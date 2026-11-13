# H757 board: how the traffic of the host link travels with the board as SPI master

Status: DECIDED, option A below. The link is **SPI3** of the H757 (SCK PB3, MISO PB4, MOSI PB5, NSS PA15 on the board), the H757 is the master, and the frame of `SpiOtaFrame_t` (128 bytes, sequence number and CRC-16) is kept.

## The question (answered)

On the bench boards of today the host (a Raspberry Pi CM5) is the SPI master and the bootloader of the CM4 is the SPI slave: the host clocks a 128-byte frame
(`SpiOtaFrame_t`, with a sequence number and a CRC-16) and the board answers in the same transaction. Every command goes through that one frame format: queries of the version and the
error counters, the readback of the backup, the update itself, and the relay to the other core (`target_tier`) and to the slots of the stack bus (`target_tier` and `target_slot`).

On the H757 board the board is the SPI MASTER towards the NVIDIA Jetson Orin NX (profile `h757_target`, `host.spi_role`): the board starts the transactions and the host raises READY
when it has something to say. What exists for that role today is `spi_boot_master.h` and `spi_boot_master_fsm.h`: the frames and the state machine with which the bootloader asks the
host for a signed image (HELLO, OFFER, GET_PAGE, PAGE, GET_TOKEN, TOKEN, DONE, REFUSE). That covers the download of an image and nothing else. The rest of the traffic above has no definition on a
link where the board is the master. Which one of the three options below is used decides how the gateway of the CM7 is written.

## Option A (chosen): keep the 128-byte frame, change only who clocks

The frame `SpiOtaFrame_t` and its seal (sequence number and CRC-16) stay as they are. The board runs a master transaction when the host raises READY, and also when the board itself has something to send
(a heartbeat, an answer); every transaction carries one frame in each direction, so the host's request arrives in the same transaction in which the board sends its last answer or an idle frame.
Only the transport changes: the dispatch of frames, the relay to the other core and to the stack slots, and all the handlers are used as they are.

- For: the least new code, and the code that was written and tested for the slave link is reused; a tool on the host that speaks the frame today only has to learn to raise READY and answer a clock it does not start.
- Against: an answer arrives one transaction after the request (the master has to clock again to read it), so a request-and-answer takes two transactions; the board must clock often enough while the host is waiting.

## Option B: a new command protocol on the master link

Define, in the style of the boot master (small frames, one type byte, a length and a CRC-32), a set of commands for everything: version, counters, readback, relay, telemetry.

- For: it fits the master role naturally, with a request and its answer in a defined order, and it can be made smaller than 128 bytes.
- Against: every handler and the host side have to be rewritten and tested again; two protocols for the same chip during the change.

## Option C: the signed image uses the master link and the rest stays as it is

The image download uses the boot master; the rest of the traffic keeps the slave link with the host as master.

- For: nothing of the existing relay changes.
- Against: it needs two SPI roles on one host link, which one physical link cannot have; it does not fit a board that is the master.

## What is done while the question is open

The pieces that do not depend on it are written and tested on the host: the frames and the state machine of the boot master, the operations on the mailbox between the cores (either core can be the gateway),
the roles of the bootloaders read from the platform profile, the clock set-up of the CM7 and the start-up of the external memory. The gateway of the CM7 (the SPI3 master, the stack bus and the relay to the CM4) waits for this answer.
After it: the transport layer with a double of the host (master with READY, slave as today), the gateway of the CM7 under the H757 profile reusing the dispatch of the CM4, the backend of the CM4, and the default platform.

Nothing described here has run on a board.
