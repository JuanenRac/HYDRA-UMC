# The bootloader as SPI master

The board of production is the SPI master towards the NVIDIA Jetson Orin NX (the CM5 stays as the pilot bench, as a slave). In this mode the bootloader of the application core asks the host for a signed image and receives it page by page. This document describes the protocol, the switch, the chain of trust and the phases in which the firmware follows. Everything below is written against a host model (`tools/spi_boot_master.py`, `tests/test_spi_boot_master.py`); no board and no Jetson has run it.

## Who starts

The SPI master clocks and the slave warns: the bootloader polls the READY line, which the host raises when it has something to ask for (an update to offer). The bootloader never waits inside a transaction and gives up after a number of polls (`ready_timeout_polls`). The telemetry and the update both start from the board.

## Switch

`HYDRA_BOOT_SPI_MASTER` in the firmware build, `BootConfig.spi_master_enabled` in the model. **Off by default**: with it off the bootloader does not touch the SPI link and the update goes on over CAN as before. The mode adds a way to receive an image; it never removes one.

## Frames

One frame per transaction, little endian, the answer on the next transaction once READY is high:

| Field | Size | Meaning |
|---|---|---|
| sync | 1 | `0x53` |
| type | 1 | master to host: `0x01` HELLO, `0x02` GET_PAGE, `0x03` GET_TOKEN, `0x04` DONE. host to master: `0x81` OFFER, `0x82` PAGE, `0x83` TOKEN, `0x8F` REFUSE |
| sequence | 2 | the request number; an answer with another number is never believed |
| length | 2 | at most 260 (a page of 256 bytes and its 4-byte offset) |
| payload | length | |
| CRC-32 | 4 | of every byte before it |

The CRC is checked before anything else is believed. A damaged frame is asked again up to 3 times, then the attempt ends and the staging slot is emptied.

## The exchange

1. HELLO carries the hardware identifier, the link address, the installed version, a fresh nonce (the one a downgrade token is made for) and the version of the bootloader.
2. OFFER answers with the size, the version, the minimum bootloader the image needs and the target name. The bootloader refuses **before a byte is fetched** a size that cannot be an image or does not fit the application slot, a target that is not this board and a minimum bootloader newer than itself.
3. GET_PAGE / PAGE, 256 bytes at a time, into the staging slot. Nothing in it is valid and nothing is run from it.
4. GET_TOKEN only when the image is older than the installed one: the signed downgrade token for this pair of versions and this nonce.
5. The whole image goes through `image_header.verify_image`, **the same function that decides over CAN**: the Ed25519 signature first, then the CRC, the hardware identifier, the link address, the vectors, the initial stack pointer inside RAM and the anti-rollback rule. Only when it returns is the slot marked valid.
6. DONE tells the host. A host that does not hear it does not undo a verified image.

The checks of step 2 only refuse earlier; none of them replaces step 5, and a signature, a downgrade rule, a hardware identifier or a minimum bootloader is never weaker over SPI than over CAN.

## What the tests pin down

A changed byte anywhere (header, image, signature) leaves nothing valid; another key, another hardware, another link address, a stack outside RAM, an equal or older version without a token, and a token for another pair, another nonce or another key are all refused; a damaged page is asked again and one that stays damaged ends the attempt; a READY line that never rises or drops halfway is a timeout; an answer for another request is never believed; an image shorter than offered fails; a second attempt starts from an empty slot; and a fuzz of the link (fixed seed) never leaves an unverified image marked valid.

## Phases of the firmware

Each phase is one commit with its own temporary branch and CI run in Actions; nothing is compiled on the PC.

1. **Documentation H745 to H757** (README in the eight languages from the documentation sources, architecture, bench tests, switches, F-RAM erase, the technical note).
2. **Rename** `src/mcu_stm32h745` to `src/mcu_stm32h757` with the linkers, build scripts, tools, tests and workflows, in its own commit.
3. **The profile in the firmware**: a header generated from `docs/platform_profiles.json` (platform, `HYDRA_SPI_ROLE` with `master` as the default and `slave` for the CM5 path, the owner of each peripheral, the memory map of `docs/H757_MEMORY_MAP.md`) and a host test of the header in the table of `tools/host_tests.py`.
4. **The bootloader**: the SPI1 master driver and the frames above in the bootloader of the application core, behind `HYDRA_BOOT_SPI_MASTER`, with the same `update_verify` chain of trust, a double of the host link in the host tests and the golden of the frames of this model.
5. **New peripherals**, one per commit, each with a table and a host double: SPI5 (the NOR flash), SPI6, FDCAN2, I2C1 to I2C3, ADC, DAC, USB host, FMC with the SDRAM, LTDC and DMA2D.

## Physical test (not done)

A Jetson Orin NX and an H757 board, with a logic analyser on SPI1 (SCK, MOSI, MISO, NSS) and the READY line: offer a signed image and see the board fetch it page by page and verify it; flip one byte in the image on the Jetson side and see the board refuse it with nothing staged; offer an older image with and without a token; cut the READY line halfway and see the attempt end and the board keep running its application; check with the switch off that the SPI link carries no frame of this protocol.
