# HYDRA-UMC System Architecture

**Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>**
**License:** CC BY-SA 4.0 (documentation) - see this repo's own README.md for the
full licensing split (firmware GPL-3.0, hardware CERN-OHL-S v2, docs CC BY-SA 4.0).

This document is the missing piece between `README.md` (which stops at "the
STM32H745 talks to up to 8 slave modules over FDCAN1") and the actual robot
cell: what those 8 slaves *are*, how a URTC tool head (and its own optional
expansion board) reaches the rest of the system, and how firmware gets onto
any of these boards without a JTAG/SWD probe or a USB-CAN dongle physically
plugged into anything.

Every fact below is marked either **CONFIRMED** (already built/documented
elsewhere in this repo or in the sibling `URTC` repo) or **PROPOSED** (a
design filling a real, previously-undocumented gap - consistent with
everything already built, but not yet implemented or hardware-verified). Do
not treat a PROPOSED item as settled fact until it's been implemented and
verified against real hardware.

---

## 1. The four flashable/testable tiers

```text
+-------------------+     +-------------------+     +--------------------+     +-------------------+     +-------------------+
|  COMPUTE MODULE 5  | SPI |   STM32H745ZIT6   |FDCAN|  ROBOT CONTROLLER  | CAN |    URTC TOOL HEAD  | I2C |  ADVANCED EXPANSION|
|  (HYDRA-UMC-STUDIO |---->|  "Kinematic Brain" |1    |  BOARD (x1-8, one  |---->|  STM32F303CCT6,    |---->|  (optional)        |
|  dashboard, Linux) |<----|  S-curve motion,   |<----|  per robot; 6-axis |<----|  see URTC's own    |<----|  STM32F303CBT6     |
|                     | 50  |  FDCAN1 STACK A    |STACK|  STEP/DIR/EN,     |     |  docs/CANBUS.TXT)  |     |  (only when         |
|                     | MHz |  master (up to 8   |A    |  endstops. Own    |     |                    |     |  expansion_board_  |
|                     |     |  robot slots)      |     |  STM32G474RET6,   |     |                    |     |  type is 3 or 4 -  |
+-------------------+     +-------------------+     |  2x FDCAN - one    |     +-------------------+     |  see §5)           |
    Tier 0                     Tier 1                |  uplink (Tier 1->2)|          Tier 2                +-------------------+
                                                       |  one downlink      |                                    Tier 3
                                                       |  (Tier 2->3)       |
                                                       +--------------------+
```

Tier numbering here matches "how many hops from the CM5", which is what
Flasher/Tester's own target picker uses (`canOta.ts`'s `CanOtaTier`):

- **Tier 0 - Kinematic Brain (STM32H745ZIT6), reached directly over SPI:**
  CONFIRMED bus (Tier 1's own transport, see below), **PROPOSED** flashing
  protocol - see section 3.
- **Tier 1 - Robot Controller Board (STM32G474RET6), reached over FDCAN1
  "STACK A":** CONFIRMED bus (1x native FDCAN1 on the STM32H745, up to 8
  slaves, 1 Mbps arbitration / 5-8 Mbps data, TCAN1044/TJA1443 transceiver -
  README.md section 6). MCU **CONFIRMED: STM32G474RET6** (Cortex-M4 @ 170
  MHz, LQFP-64, 512 KB flash, 3x FDCAN peripherals - 2 used: one uplink to
  the STM32H745's own FDCAN1, one downlink to that robot's own URTC Tool
  Head). Addressing scheme **PROPOSED** - see section 4.
- **Tier 2 - URTC Tool Head (STM32F303CCT6), reached one hop further, relayed
  through the Robot Controller Board:** CONFIRMED board/firmware (see the
  sibling `URTC` repo, `docs/CANBUS.TXT`). The physical CAN link from the
  Robot Controller Board to it, and the relay behavior on the Robot
  Controller Board's own side, are **PROPOSED** - see section 5.
- **Tier 3 - Advanced Expansion Board (STM32F303CBT6), only present when a
  robot's URTC head has `expansion_board_type` 3 or 4 installed, reached two
  hops further, relayed through both the Robot Controller Board AND the URTC
  head:** CONFIRMED board/firmware and CONFIRMED relay protocol on the URTC
  head's own side (`URTC/docs/CANBUS.TXT` IDs `0x210`-`0x221`,
  `URTC/docs/EXPANSION.TXT` for the 6 expansion board variants). Reaching it
  from HYDRA-UMC-STUDIO needs no NEW protocol design beyond section 5's own
  tunnel - see section 5.

---

## 2. RTOS choice per tier

**CONFIRMED (by the project owner) - FreeRTOS runs on both Tier 0 (both
cores) and Tier 1.** Neither Tier 2 (URTC Tool Head) nor Tier 3 (its
Advanced Expansion Board) run an RTOS - both are CONFIRMED, already-shipping
bare-metal superloop designs (see the sibling `URTC` repo) that this project
doesn't change.

- **Tier 0 - STM32H745, both cores:** CM7 and CM4 each run their OWN
  FreeRTOS instance - this is a dual-core AMP (asymmetric multiprocessing)
  chip, not SMP, so there is no shared kernel state or scheduler between the
  two; each core's own tasks, queues, and heap are entirely private to it.
  Cross-core communication (once designed - see section 5's own note on the
  D3-domain SRAM4 region reserved for this) will need its own explicit IPC
  mechanism (a HAL/FreeRTOS-agnostic shared-memory mailbox, most likely),
  not anything FreeRTOS provides for free across cores.
- **Tier 1 - STM32G474:** a single FreeRTOS instance (one core, no AMP/SMP
  question).
- **Bootloaders (all 3 - CM7, CM4, G474) stay bare-metal, no FreeRTOS.** A
  bootloader's job (receive firmware, verify, jump) doesn't need a
  scheduler, and keeping it minimal is itself a safety property - less code
  running before anything has been verified, easier to audit, one less
  moving part that could itself have a bug bricking a board. This mirrors
  URTC's own bootloader, which is bare-metal too.

Implementation status: `../firmware/mcu_stm32g474/`, `../firmware/mcu_stm32h745/CM7/`,
and `../firmware/mcu_stm32h745/CM4/` each have a real, verified-compiling
FreeRTOS skeleton (one task, GPIO toggle - proves the toolchain+RTOS
pipeline itself works, not real firmware) - see `docs/COMPILE_STM32G474.TXT`
and `docs/COMPILE_STM32H745.TXT` for exactly what that does and doesn't
include yet (real clock config, real tasks, cross-core IPC are all still
open).

---

## 3. Tier 0: flashing the Kinematic Brain itself (STM32H745, over SPI)

**PROPOSED** - no bootloader exists yet for the STM32H745 side of this link.

The STM32H745 isn't reached over CAN at all - it's wired directly to the CM5
over the same SPI1 link Tier-1 telemetry already uses (`HYDRA_DATA_READY`
handshake, 128-byte frames, README.md section 10). Proposal: reuse that same
physical transport for firmware updates too, carrying the **same command
vocabulary** Tiers 1-3 below reuse from URTC's own proven bootloader
(ENTER_BOOTLOADER, START_UPDATE, DATA, PAGE_ACK, END_UPDATE, STATUS,
HEARTBEAT, HMAC_CHUNK, VERSION_QUERY/RESPONSE, TEC/REC, ALLOW_DOWNGRADE,
BACKUP_READ) as a **frame-type byte** inside the existing SPI frame header,
instead of as a CAN ID - SPI frames don't have IDs to overload the way CAN
frames do, but the same command *semantics* still apply directly, including
the same anti-bricking discipline (verify into a backup slot before copying
into the running one). This is the only tier that talks to the CM5 without
crossing FDCAN1 at all, so it needs no STACK-A slot addressing.

---

## 4. Tier 1 addressing: which of 8 Robot Controller Boards is a frame for?

**PROPOSED** - not yet implemented on the STM32H745 or any Robot Controller
Board firmware.

FDCAN1 "STACK A" is one shared bus carrying up to 8 Robot Controller Boards.
URTC's own protocol (`CANBUS.TXT`) assumes exactly one board per CAN segment,
so its ID blocks (`0x0xx`-`0x2xx` runtime, `0x7Fx` bootloader) are fixed,
un-addressed constants. STACK A needs a slot dimension URTC's protocol was
never designed to carry. Proposal: give each slot `N` (0-7, corresponding to
A1-A8) its own 32-ID window:

```text
CAN_ID_STACKA_BASE = 0x600
Slot N window       = 0x600 + (N * 0x20)  ..  0x600 + (N * 0x20) + 0x1F
```

Within each slot's window, the **same relative offsets URTC's own bootloader
already uses** are reused verbatim for the Robot Controller Board's own
firmware (offsets `+0x00`..`+0x0F` - a direct, 1:1 re-based copy of URTC's
own `0x7F0`-`0x7FF`, so its bootloader state machine can be lifted almost
directly from URTC's proven implementation instead of designed from
scratch), plus 2 new axis-telemetry IDs and the Tier-2 relay pair from
section 5 below:

| Offset | Purpose                              | Mirrors URTC's own (fixed) ID |
|--------|---------------------------------------|--------------------------------|
| +0x00  | ENTER_BOOTLOADER                      | 0x7F0 |
| +0x01  | START_UPDATE (size + HardwareID)      | 0x7F1 |
| +0x02  | DATA (8-byte firmware chunk)          | 0x7F2 |
| +0x03  | PAGE_ACK                              | 0x7F3 |
| +0x04  | END_UPDATE (CRC32 + version)          | 0x7F4 |
| +0x05  | STATUS (incl. verify-fail reason)     | 0x7F5 |
| +0x06  | HEARTBEAT (status + % progress)       | 0x7F6 |
| +0x07  | HMAC_CHUNK (x4, 32 bytes total)       | 0x7F7 |
| +0x08  | VERSION_QUERY                         | 0x7F8 |
| +0x09  | VERSION_RESPONSE (app/bootloader)     | 0x7F9 |
| +0x0A  | BOOTLOADER_VERSION (bootloader-only)  | 0x7FA |
| +0x0B  | TEC (transmit error counter)          | 0x7FB |
| +0x0C  | REC (receive error counter)           | 0x7FC |
| +0x0D  | ALLOW_DOWNGRADE (bypass anti-rollback)| 0x7FD |
| +0x0E  | BACKUP_READ_REQUEST                   | 0x7FE |
| +0x0F  | BACKUP_READ_RESPONSE                  | 0x7FF |
| +0x10  | AXIS_STATUS (6x endstop bits + fault) | *(new - no URTC equivalent)* |
| +0x11  | AXIS_STEP_TELEMETRY                   | *(new - no URTC equivalent)* |
| +0x12  | RELAY_SEND (tunnel to Tier 2 - see §5)| *(new - no URTC equivalent)* |
| +0x13  | RELAY_RECV (tunnel from Tier 2 - §5)  | *(new - no URTC equivalent)* |
| +0x14..+0x1F | Reserved for future Robot Controller Board features | |

Same anti-bricking discipline as URTC's own bootloader: a firmware image is
only copied into the running slot after a full CRC32 + HMAC-SHA256 verify
against a backup/staging slot, so an interrupted or corrupted CAN-OTA update
leaves the previously-working firmware intact.

---

## 5. Tiers 2-3: reaching the URTC Tool Head, and its own optional Advanced
   Expansion Board, through the Robot Controller Board

**PROPOSED**, modeled directly on a pattern URTC's own firmware already
implements one level down: its expansion-slave I2C bridge (`CANBUS.TXT` IDs
`0x210`-`0x221`, see `EXPANSION.TXT` for the 6 expansion board variants),
which relays bootloader and register traffic to a second MCU it has no
direct CAN access to, unmodified and un-reinterpreted, without needing a
dedicated fixed ID for every possible downstream command.

The Robot Controller Board does the same thing one hop earlier, but as a
**generic, ID-agnostic tunnel** (`+0x12`/`+0x13` from section 4's table)
rather than a fixed 1:1 ID mapping - URTC's own protocol alone spans over a
hundred distinct IDs (`0x000`-`0x2FF` runtime, `0x7F0`-`0x7FF` bootloader),
far more than a 32-ID slot window could enumerate individually:

- **RELAY_SEND (+0x12):** payload = target CAN ID (2 bytes, big-endian) +
  DLC (1 byte) + up to 5 data bytes. Queues a frame to be sent on the Robot
  Controller Board's own second CAN controller, addressed exactly as read
  from the payload. A logical operation needing the full 8 data bytes URTC's
  own frames carry is split across 2 consecutive RELAY_SEND frames - the
  same "multiple 8-byte relay frames per logical operation" pattern URTC's
  own HMAC_CHUNK (`0x7F7`/`0x212`, 4 frames) and thermal-pixel-chunk
  transfers (`0x253`, 4 frames) already establish, not a new one.
- **RELAY_RECV (+0x13):** polled by the operator (HYDRA-UMC-STUDIO), drains
  the Robot Controller Board's own FIFO of frames captured off its second CAN
  controller since the last RELAY_RECV, oldest first - pull-based rather
  than pushed, to keep STACK A's own bandwidth budget under the operator's
  control instead of the Robot Controller Board's.

This tunnel is intentionally **target-ID-agnostic**: reaching the URTC
head's own bootloader (`0x7F0`-`0x7FF`) or its runtime tool protocol
(`0x000`-`0x2FF`) is just a RELAY_SEND/RELAY_RECV pair with the matching
target ID in the payload - the Robot Controller Board never needs to
understand what's on the other end of it. This is also, for free, exactly
how **Tier 3 (the Advanced Expansion Board)** is reached: its own
STM32F303CBT6 has no CAN peripheral of its own at all - URTC's firmware
already bridges to it over a local I2C bus using IDs `0x210`-`0x221`
whenever it's reached directly (`expansion_board_type` 3 or 4 only - see
`EXPANSION.TXT`). Tunneling one of *those* IDs through the same
RELAY_SEND/RELAY_RECV pair reaches the expansion board's own bootloader with
**zero additional protocol design** - it fully piggybacks on URTC's own
already-implemented second relay hop, the same way Tier 2 piggybacks on
URTC's own bootloader being unmodified.

Net effect for HYDRA-UMC-STUDIO's Flasher/Tester: flashing or testing a URTC
head, or its own Advanced expansion board, is the *same* URTC protocol as if
it were connected directly, tunneled through 1 or 2 extra relay hops instead
of zero. The UI shows this hop count to the operator (`hopDescription()` in
`canOta.ts`) rather than hiding it, since a stall at any one tier needs to be
diagnosable independently. The Advanced-expansion target is only offered at
all when that robot's own URTC head last reported `expansion_board_type` 3
or 4 (queried the same way URTC Flasher already does today, per
`EXPANSION.TXT` section 4-5) - there's no point offering to flash a chip
that isn't there.

---

## 6. What already exists vs. what this fills in

| Piece | Status |
|---|---|
| CM5 <-> STM32H745 SPI transport | CONFIRMED, documented (README.md §10) |
| STM32H745's own SPI firmware-update protocol (Tier 0) | PROPOSED (this document, §3) - no bootloader exists yet |
| STM32H745 <-> Robot Controller Board FDCAN1 bus (electrical) | CONFIRMED, documented (README.md §6) |
| Robot Controller Board slot addressing on that shared bus | PROPOSED (this document, §4) |
| Robot Controller Board's own MCU identity | **CONFIRMED: STM32G474RET6** (LQFP-64, 512 KB flash, 3x FDCAN - 2 used, see §1). |
| Robot Controller Board's own bootloader firmware | Does not exist yet. §4's ID map is meant to make writing it straightforward, not a substitute for writing it. |
| Robot Controller Board -> URTC Tool Head CAN link (electrical) | Described by the project owner; not yet on a schematic in `hardware/` |
| Robot Controller Board <-> URTC Tool Head relay tunnel | PROPOSED (this document, §5) |
| URTC Tool Head firmware + protocol | CONFIRMED, fully implemented - see the `URTC` repo, `docs/CANBUS.TXT` - bare-metal, no RTOS (§2) |
| URTC's own Advanced Expansion Board (STM32F303CBT6) + its I2C relay | CONFIRMED, fully implemented - see `URTC/docs/EXPANSION.TXT` and `CANBUS.TXT` §"EXPANSION SLAVE BRIDGE" - bare-metal, no RTOS (§2) |
| Reaching the Advanced Expansion Board from HYDRA-UMC-STUDIO | PROPOSED, but needs no new protocol beyond §5's tunnel - piggybacks on URTC's own existing relay |
| FreeRTOS on Tier 0 (both cores) and Tier 1 | CONFIRMED design decision (§2). Implementation: a real, verified-compiling skeleton (one task, GPIO toggle) exists for all 3 - `firmware/mcu_stm32g474/`, `firmware/mcu_stm32h745/CM7/`, `firmware/mcu_stm32h745/CM4/` - not yet the real tasks. |
| HYDRA-UMC-STUDIO Flasher/Tester UI | Implemented against a **simulated (mock)** transport that follows this document's addressing scheme for all 4 tiers, including GitHub-release firmware download (currently wired for the `URTC` repo only) - see that repo's own README for current status. No real transport (SPI or CAN) is implemented yet - no STM32H745/Robot-Controller-Board firmware exists yet to talk to. |

---

## 7. Superseded documents

`docs/HYDRA-UMC_TECHNICAL.txt`, `docs/HYDRA-UMC_BOM.txt`, and
`docs/HYDRA-UMC_PINOUT.txt` describe an earlier board revision
(STM32H757BIT6/LQFP-208, onboard TMC5160A drivers wired directly to robots,
ESP32-C3, USB hub, Ethernet PHY) that this document's tiered
STM32H745/Robot-Controller-Board architecture replaced. They're marked
superseded at the top of each file and kept for historical reference only -
do not use them as a source of truth for new work.
