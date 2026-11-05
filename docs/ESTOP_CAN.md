# Emergency stop on the stack bus: identifier 0x001

Status: **firmware written and tested on the host; nothing in this page has run on a board.** The controller board and the kinematic brain
implement it (see Firmware below). `docs/bus_contract.json` (section `emergency_stop`) holds the numbers, `tools/estop_can.py` is the
reference of the frame and of the state machine, `tests/estop_vectors.json` holds the frames and the scenarios that the firmware reproduces
(`tests/test_estop_can.c` replays all of them against `estop_can.h`), and `tests/test_estop_contract.py` and
`tests/test_node_headers_parity.py` keep all of them consistent. This page is the one place where the contract is written; the other
repositories of the family link here instead of copying it.

This is **not a certified safety function.** It is an additional, fast, software path. It never replaces a hardware emergency stop
and no document of this repository may present it as one.

## Identifier and priority

- **0x001**, standard 11-bit identifier, classic data frame, 8 data bytes, no remote frame (the hardware filters already reject remote frames).
- CAN arbitration lets the lowest identifier win, so 0x001 beats every other frame of the family: the slot windows start at 0x600 and
  the tool head protocol starts at 0x095. 0x000 is left unused on purpose.
- It is one identifier for the whole stack, **not** inside a slot window (`slot_base + slot * 32 + offset`). The only slot message the
  stop adds is `ESTOP_STATE` (offset 0x1D, sent by a controller board), see below.
- It travels on STACK A (FDCAN1) and is forwarded unchanged by every controller board to its tool head on the Tier 2 downlink (FDCAN2).
  The forwarding is done in firmware, one frame in, one frame out, never inside the relay tunnel.
- At 1 Mbit/s a worst-case frame (8 data bytes, maximum stuffing, interframe space) is 135 bits, 0.135 ms. A stop waits at most for the
  frame already on the wire, so it reaches every node in under 0.3 ms whatever the bus load.

## Frame

| Byte | Field | Meaning |
|---|---|---|
| 0 | `kind` | `0x01` TRIP, `0x02` RELEASE, `0x03` ALIVE |
| 1 | `detail` | TRIP: reason. RELEASE: `0x00`. ALIVE: `0x00` not tripped, `0x01` tripped |
| 2 | `source` | `0x00` kinematic brain, `0x01`..`0x08` slot board, `0x10` CM5 bridge |
| 3 | `epoch` | counts stops (modulo 256): +1 each time the stack goes from running to tripped |
| 4-5 | `sequence` | 16 bits, little endian, +1 on every frame the sender transmits on 0x001 (modulo 65536) |
| 6 | `kind` complement | `kind` XOR `0xFF` |
| 7 | `detail` complement | `detail` XOR `0xFF` |

Reasons of a TRIP: `0x01` operator, `0x02` stop-all from the server, `0x03` hardware input, `0x04` safety zone, `0x05` host link lost,
`0x06` controller fault, `0x07` bus fault, `0x08` power loss, `0xFF` unspecified.

**Who may send what.** Any node may send a TRIP. Only the kinematic brain (source `0x00`) sends RELEASE and ALIVE.

**Authentication: none, on purpose.** CAN has no source address and 8 bytes leave no room for a MAC. More important, the direction that
matters is protected the other way round:

- A stop must never be refused. A forged or damaged frame can only cause a stop, which is the safe outcome. For that reason **any frame
  on 0x001 that is not a well-formed RELEASE or ALIVE is treated as a TRIP**, including a wrong length, a wrong complement, an unknown
  kind, an unknown reason, a RELEASE or ALIVE claiming a source other than the brain.
- The dangerous direction (ending a stop) is not decided by one frame: a RELEASE only moves a node to READY, never to running, and needs the
  conditions below. An attacker with access to the bus already controls every motor directly; keeping the bus inside the enclosure is the
  protection, not a signature on this frame.
- The complements catch a sender that is stuck or half-written. The 15-bit CAN CRC already covers line errors.

## Meaning and rules for a node

Two states: **LATCHED** (drivers off) and **READY** (drivers may be enabled by the normal enable command, which is still a separate action).

- **Power-up is LATCHED.** The brain also starts LATCHED and announces it (ALIVE tripped) until a person releases.
- **TRIP, a malformed frame, bus-off, or an ALIVE that says tripped: LATCHED at once**, drivers off, whatever the epoch or sequence. An
  ALIVE that says tripped recovers a TRIP the node never heard.
- **ALIVE not tripped** refreshes the liveness timer **only if its sequence is newer** (modular order). A sender that repeats the same
  sequence (frozen) does not keep the node alive.
- **RELEASE** is accepted only when all of these hold: the node is LATCHED; the epoch equals the epoch of the stop it knows (learned from a
  TRIP or an ALIVE); the sequence is newer than the last one it saw (a replayed RELEASE is ignored); and an ALIVE was received within the alive
  timeout (the master is really there). Then, and only then, the node becomes READY. It does **not** enable anything.
- **Loss of the master:** a READY node with no accepted ALIVE for the alive timeout becomes LATCHED. The bus coming back never releases anything.
- **Manual and acknowledged re-arm.** The brain sends RELEASE only after a person asked for it in an authenticated administrative action
  (Studio or the control apps, with confirmation), never on its own. Each controller board answers on `ESTOP_STATE`
  (`slot_base + 0x1D`, 4 bytes: state `0` LATCHED / `1` READY, epoch, last accepted sequence low and high). The brain repeats RELEASE
  every `release_repeat` ms; when every expected board reported READY it reports the release as done to the CM5; if one does not within
  `release_ack_timeout` ms it sends a TRIP again (back to LATCHED) and reports which board did not acknowledge. After a RELEASE a person
  still enables the drivers with the normal command.

### Timing (proposals; the firmware will read them from the configuration file)

| Name | Value (ms) | Meaning |
|---|---|---|
| `trip_repeat` | `5` | a TRIP is repeated at this period while the stack is tripped |
| `alive_period` | `20` | the brain sends ALIVE at this period (about 0.5 % of the bus) |
| `alive_timeout` | `100` | silence for this long latches a READY node (five periods: one lost frame never does) |
| `node_reaction_max` | `10` | from the stop frame received to the drivers disabled |
| `release_repeat` | `20` | period of the RELEASE repeats |
| `release_ack_timeout` | `500` | wait for the `ESTOP_STATE` of every board before the release is cancelled |

## Safe state of each node type

| Node | On TRIP, malformed frame, bus loss or silence of the master |
|---|---|
| Kinematic brain (H745) | stops generating setpoints, empties the motion queue, refuses motion until READY and an explicit enable, tells the CM5 over SPI, keeps repeating TRIP |
| Controller board (G474) | de-asserts the enable of every motor driver and stops the pulse generation (stop category 0), switches valves, pumps and other outputs off, forwards the frame to its tool head, reports `ESTOP_STATE` |
| Tool head (URTC) | heaters, laser, solder, spindle and ultrasonic outputs off. Requires the head firmware to honour 0x001, see Pending |
| CM5 / server | announces `SAFE_STOP` as in `GLOBAL_STOP.md` of HYDRA-UMC-SERVER; a stop-all from the server asks the brain to send a TRIP |

## Relation with the other stops

- **Hardware emergency stop (a physical chain that cuts the driver enable or power):** independent of this path and it outranks it. A
  hardware stop must also be visible to the brain, which then sends a TRIP with reason `0x03` so the rest of the stack follows. If the bus is
  down, the hardware chain still works; if the hardware chain is open, a RELEASE cannot make anything move.
- **Dead-man of a jog (control apps and server):** the operator's heartbeat only slows or stops one axis in the server and never outranks a
  stop; its expiry does not send a TRIP. ALIVE is a different thing: the liveness of the brain towards the nodes on the bus.
- **Tool head multi-node SAFE_STOP of URTC (identifier 0x402):** a stop inside the head bus of URTC. A head that honours 0x001 must treat it as
  at least as strong as that one. The two identifiers are not merged.

## Pending

These need a person to decide or hardware that does not exist yet; none is guessed here.

1. **Stop category per axis.** Category 0 (drivers off at once) is the default and what the bench row asks for. An arm carrying a load can
   fall when its drivers are cut. Question for the owner: should some axes brake or ramp down first (category 1) and which, and is there a
   holding brake output on the boards?
2. **Physical emergency stop input.** No document of this repository describes one on the carrier or on the STACK A connector. Question for
   the owner: is there a physical emergency stop input, where does it connect, and does it cut the driver enable directly?
3. **Tool heads.** Honouring 0x001 is a change to the head firmware of URTC. Question for the owner: should the URTC heads adopt it?
4. **Timing.** The values above are proposals; the bench rows 17, 18, 20 and 21 of `docs/BENCH_TESTS.md` measure them.
5. **The SPI command.** The brain has the calls that stop the stack and start a release (`KinematicBrainCan_EstopTrip`,
   `KinematicBrainCan_EstopRequestRelease`), but the SPI command that asks for them from the CM5 is not defined: it changes the protocol between
   the CM5 and the CM4 and goes with a coordinated update of both. Until then nothing but the firmware itself (a TRIP heard on the bus) stops the
   stack, and a release is only started by code.
6. **The bench.** None of the firmware below has run on a board: rows 17, 18, 20 and 21 of `docs/BENCH_TESTS.md` measure it.

## Firmware

- `estop_can.h` (the same file in `src/mcu_stm32g474/` and `src/mcu_stm32h745/CM4/`): the frame, the decision and the node state machine.
  `estop_brain.h` (CM4): what the master sends and when. Neither touches the hardware; `tests/test_estop_can.c` runs the vectors and a bus with
  a brain and several nodes (power-up, release, a stop, a release that one board does not confirm, a stop during a release, wrap of the clock).
- Controller board (`RobotControllerRelay.c`): the node starts LATCHED with the driver enable outputs forced off; a frame on 0x001 (accepted by
  the hardware filter) goes to the state machine, the outputs are forced off in the same pass when it is latched and the frame is passed on
  unchanged to the tool head on FDCAN2. A bus-off latches the node and restarts the controller; the silence of the master latches a READY node.
  `ESTOP_STATE` is sent on a change, after every RELEASE and every 100 ms. The normal enable command has to ask
  `RobotControllerRelay_RequestDriversEnable()`, which refuses while LATCHED.
- Kinematic brain (`KinematicBrainCan.c`): starts stopped and repeats TRIP; the STACK A task runs every millisecond (the slot queries wait at most
  20 ms and keep sending the stop frames while they wait).
- Tool heads (URTC) do not honour 0x001 yet: that is the head firmware of another repository.

## Related

- `docs/bus_contract.json`, `tools/estop_can.py`, `tools/can_ids.py` (`ESTOP_STATE`), `tests/estop_vectors.json`, `tests/test_estop_contract.py`
- `docs/CANBUS_STM32G474.md`, `docs/CANBUS_STM32H745.md`, `docs/BENCH_TESTS.md`
