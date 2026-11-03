# Bench tests

Every test below is **not executed**. The tunnel between the Robot Controller and the Tool Head (the CAN link to URTC), the FDCAN clock and bitrate and the over-the-air update of a URTC head exist as code and as host tests; none has been checked on a bus with a real URTC head. The architecture notes mark the tunnel "in code, not working yet" until these steps pass. Write a result only after a person ran the step.

## What the software already covers (no board needed)

- `python tools/host_tests.py` builds and runs the C host tests with the compiler that is installed.
- The simulated URTC head in HYDRA-UMC-DOCS (`fake_urtc.py`) answers the identification request and the bootloader identification frames with the bytes of the shared vectors, over a simulated bus and over a SocketCAN socket; it has not been run over a real `vcan` on Linux.

## Setup

- The Robot Controller board (STM32G474) on its CAN connector, a URTC head board with the matching firmware, a 24 V supply with a current limit and both ends of the bus terminated with 120 ohm.
- A CAN analyser (a second adapter and `candump`, or a scope with a CAN decoder) on the bus. The actuators of the head stay disconnected.
- The CAN identifiers agreed in writing with the URTC owners before step 3.

## Steps

| # | Step | Expected result | Result |
|---|------|-----------------|--------|
| 1 | Measure the FDCAN2 clock and the bit timing on the board. | The nominal and data bitrates are the ones in the configuration; write the measured values. | not executed |
| 2 | Power both boards and look at the bus. | Frames are seen and no error counter rises in ten minutes. | not executed |
| 3 | Send the identification request (0x110) from the controller. | The head answers 0x111 with its identity; any other identifier seen is recorded. | not executed |
| 4 | Run the bootloader identification (0x7F8) through the controller. | The answers 0x7F9, 0x7FA and 0x7E9 arrive with the expected bytes. | not executed |
| 5 | Relay a tool command to the head and a state back. | The command reaches the head; the state returns; a loss of the bus is reported. | not executed |
| 6 | Send a signed firmware image to the head through the tunnel, then a wrong key and a changed image. | Only the correct one is accepted; the others are refused before any write. | not executed |
| 7 | Cut the power of the head during a transfer. | The head keeps its old firmware and can be updated again. | not executed |

## Controller board, kinematic brain and host link

Every row below is **not executed** either; it needs the STM32G474 controller board, the STM32H745 kinematic brain, a CM5 host on the SPI link, a CAN analyser and a current-limited supply.

| # | Step | Expected result | Result |
|---|------|-----------------|--------|
| 8 | Run the dual-core mailbox of the H745 (CM4 to CM7 commands) for one hour at the highest command rate, with the hardware semaphore checked on a scope or a counter. | No lost, torn or repeated command; the semaphore is never held across a reset of one core. | not executed |
| 9 | Compare the CAN bitrate of the controller board with the one of the stack bus on the analyser. | Both buses show the same nominal and data bitrate; any mismatch is recorded. | not executed |
| 10 | Start the G474 bootloader from a cold power-up and measure the FDCAN kernel clock. | The kernel clock is the one the bit timing assumes and the first frame is sent without an error counter rising. | not executed |
| 11 | Wire the CAN standby pins as in the pinout note, boot, then read the option bytes. | The transceivers leave standby and the board boots from its application; the boot pin and option bytes read back as documented. | not executed |
| 12 | Send frames at the maximum rate before the scheduler of the G474 starts (reset loop, 100 cycles). | No fault and no hang in the tick or the receive interrupt. | not executed |
| 13 | Run the SPI link between the CM5 and the H745 at full rate with a frame generator, cutting a byte at random. | A damaged or repeated frame is detected and answered with a clean error; the link recovers on the next frame. | not executed |
| 14 | Ask the application on a board to enter its bootloader over the stack bus and then flash it. | The board reboots into the bootloader and accepts the signed image; a board that does not answer is reported. | not executed |
| 15 | Flash a batch of boards one after the other, with two boards sharing a board identifier. | The duplicate is reported before any write; the others finish and each reports its new version. | not executed |
| 16 | Read the memory of a production-protected board and send a wrong adoption code. | The memory cannot be read out and the wrong code is refused. | not executed |
| 17 | Send an emergency stop on the highest-priority identifier while the stack bus is at 80 per cent load. | Every motion output is disabled within the documented time; measure it with the scope. | not executed |
| 18 | Pull the supply of the tool head and of one axis board while moving. | The stack reports the loss and the motion drivers stay disabled until a person re-enables them. | not executed |
| 19 | Read the status of the eight boards through the SPI link in application mode for ten minutes. | The status, error counters and bootloader state of each board arrive without gaps. | not executed |
| 20 | Stop the alive frames of the kinematic brain on identifier 0x001 while the nodes are ready (unplug its transmitter, or freeze its sequence). | Every board disables its drivers within the alive timeout plus the reaction time, and the bus coming back releases nothing. | not executed |
| 21 | Replay a recorded release frame, then send one with the wrong epoch, then a correct one without a person asking for it. | The first two are ignored; the third only makes the boards ready and the drivers stay off until the normal enable command. | not executed |

## What this bench cannot show

Bus load with all the nodes of a real machine, noise on long cables and the timing of the safe stop; measure them on the final wiring.
| 22 | Update with a valid format 2 image, then with one bit changed in the image, the header, the key id and the signature; then with a key the table does not hold, a revoked one and (official bootloader) any key. | The valid one installs and the board resets; every other one is refused with the reason of the status frame and the installed application keeps running. The official bootloader with an empty release table refuses all. | not executed |
| 23 | Install a counter 5 image, then a newer version with counter 3, then with a valid downgrade token; then counter 7. | Counter 3 is refused with the rollback reason also with the token; counter 7 installs and becomes the floor. | not executed |
| 24 | Cut the power at several points of an update: during the data, the verification, the metadata record, the copy and the change of metadata page (G474) or sector (H745). | After every cut the board starts the old or the new application, never a damaged one. A cut at the erase of the full H745 sector loses the log (first-boot adoption with the compiled floor): record what happens. | not executed |
| 25 | Read the flash words of the metadata that a cut left half written, on the G474 (double-word ECC) and the H745 (flash word ECC). | The read of a torn record does not raise a bus fault or NMI that stops the bootloader; if it does, the record reader must handle it. | not executed |
