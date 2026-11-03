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

## What this bench cannot show

Bus load with all the nodes of a real machine, noise on long cables and the timing of the safe stop; measure them on the final wiring.
