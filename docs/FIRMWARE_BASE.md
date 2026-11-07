# The shared part of the three bootloaders

The controller board (STM32G474), the CM4 and the CM7 of the Nucleo (STM32H745) each have a bootloader. What is the same in all three lives once, in
`firmware_base/boot`, and `tools/sync_firmware_base.py` keeps the copies in the three chip directories identical byte for byte (`check` runs with the
tests; `sync` writes the base over the copies; the base is edited, never a copy). A copy stays in each chip directory because every chip has to build on
its own: the build script, the host tests and the Renode jobs name those directories.

The list of what is shared is `firmware_base/shared.json`. Two of the shared files are not compiled on their own but included by `bootloader_flash.c`
(`boot_crc32_impl.h`, the software CRC32, and `boot_meta_record_impl.h`, the checks of one metadata record), so the three flash files keep the same
order of declarations and no build file changes. `tools/sync_firmware_base.py status` prints what is shared, what differs and by how many lines.

## What still differs, and why

Compared function by function (the text of each function without comments and spacing), what is left in the chip directories differs for a reason of
the chip, not by drift:

| File | Same in all three | Same in two | Differs |
|---|---|---|---|
| `bootloader_common.h` | (macros, no functions) | | the clock, the pins, the peripherals and the memory map of each chip: addresses of the slots, the size of a page (2 KB on the G474) or a sector (128 KB on the H745), the flash word (8 or 32 bytes), the reset and clock registers |
| `bootloader_flash.c` | CRC32, the checks of a metadata record | the read, copy, write-and-verify and erase of the CM4 and the CM7 (one bank geometry, a bank at a time) differ from the G474 | erasing: pages on the G474, sectors on the H745; the geometry of the bank each core owns; the hardware calls under `flash_layer.h` |
| `bootloader_protocol.c` | the page buffer flush, the start of an update, the confirmation of a healthy start and the data frames (now one copy, `boot_update_core_impl.h`) | the check that the application is valid (the G474 against the CM4 and CM7) | how a frame leaves: FDCAN on the G474, the SPI link to the CM7 and the relay to the stack on the CM4, the mailbox on the CM7; the jump to the application (the valid range of the stack pointer is the RAM of each chip); the end of an update and the readback, which send their answers through the transport |
| `bootloader_main.c` | the fault and interrupt handlers that do nothing | the watchdog failure handler (G474 and CM4), the clock set-up (CM4 and CM7) | the entry point, the peripheral set-up (FDCAN1 on the G474, SPI1 on the CM4, the mailbox on the CM7), the dispatch of a frame, the polling of the receive loop |

`boot_update_core_impl.h` holds the page buffer flush, the start of an update, the confirmation of a healthy start and the data frames: the functions were
identical in the three protocol files, and they sit right after `boot_update_glue.h`, which already worked on the same static state. How a frame leaves
(`CAN_SendStatus`, `CAN_SendPageAck`) is declared in `bootloader_protocol.h` and stays in each file, so the order of the functions in the file no longer
matters. What is left in the protocol files differs because of the transport.

Nothing in the shared files changes a line of behaviour: the bytes of each bootloader are the ones the three chips had before, apart from the two
`#include` lines that stand where the CRC32 and the record checks used to be.
