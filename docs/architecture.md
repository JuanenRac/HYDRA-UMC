# HYDRA-UMC System Architecture

**Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>**
**License:** CC BY-SA 4.0 (documentation) - see this repo's own README.md for the
full licensing split (firmware GPL-3.0, hardware CERN-OHL-S v2, docs CC BY-SA 4.0).

This document is the missing piece between `README.md` (which stops at "the
STM32H745 talks to up to 8 slave modules over FDCAN1") and the actual robot
cell: what those 8 slaves *are*, how a URTC tool head reaches the rest of the
system, and how firmware gets onto any of these boards without a JTAG/SWD
probe or a USB-CAN dongle physically plugged into anything.

Every fact below is marked either **CONFIRMED** (already built/documented
elsewhere in this repo or in the sibling `URTC` repo) or **PROPOSED** (a
design filling a real, previously-undocumented gap - consistent with
everything already built, but not yet implemented or hardware-verified). Do
not treat a PROPOSED item as settled fact until it's been implemented and
verified against real hardware.

---

## 1. The four tiers

```text
+-------------------+     +-------------------+     +--------------------+     +-------------------+
|  COMPUTE MODULE 5  | SPI |   STM32H745ZIT6   |FDCAN|  ROBOT CONTROLLER  | CAN |    URTC TOOL HEAD  |
|  (HYDRA-UMC-STUDIO |---->|  "Kinematic Brain" |1    |  BOARD (x1-8, one  |---->|  (+ optional        |
|  dashboard, Linux) |<----|  S-curve motion,   |<----|  per robot; 6-axis |<----|  expansion board,  |
|                     | 50  |  FDCAN1 STACK A    |STACK|  STEP/DIR/EN,     |     |  see URTC's own    |
|                     | MHz |  master (up to 8   |A    |  endstops. Own    |     |  docs/CANBUS.TXT)  |
|                     |     |  robot slots)      |     |  STM32, 2x CAN     |     |                    |
+-------------------+     +-------------------+     |  peripherals - one |     +-------------------+
                                                       |  uplink (FDCAN,    |
                                                       |  Tier 2), one      |
                                                       |  downlink (CAN,    |
                                                       |  Tier 3)           |
                                                       +--------------------+
```

- **Tier 1 (CM5 <-> STM32H745, SPI):** CONFIRMED. Full-duplex SPI1 up to 50 MHz,
  `HYDRA_DATA_READY` handshake GPIO, 128-byte telemetry frames. See README.md
  section 10.
- **Tier 2 (STM32H745 <-> Robot Controller Board, FDCAN1 "STACK A"):**
  CONFIRMED bus (1x native FDCAN1, up to 8 slaves, 1 Mbps arbitration / 5-8
  Mbps data, TCAN1044/TJA1443 transceiver - README.md section 6). The Robot
  Controller Board's own MCU is **CONFIRMED: STMicroelectronics
  STM32G474RET6** (Cortex-M4 @ 170 MHz, LQFP-64, 512 KB flash) - it exposes 3
  FDCAN peripherals (FDCAN1/2/3), of which 2 are used here: one as the Tier-2
  uplink to the STM32H745's own FDCAN1, one as the Tier-3 downlink to that
  robot's own URTC Tool Head (section 3). HYDRA-UMC-STUDIO's Config >
  CAN-OTA settings still carries this as an editable field (in case a future
  revision changes it) rather than a hardcoded constant.
- **Tier 3 (Robot Controller Board <-> URTC Tool Head, CAN):** PROPOSED
  topology (the physical existence of this link - one CAN port per robot
  toward its own head - was described by the project owner; the Robot
  Controller Board firmware that bridges it doesn't exist yet). Runs URTC's
  own **unmodified, already-implemented** protocol (`URTC/docs/CANBUS.TXT`):
  the Robot Controller Board is a transparent CAN-to-CAN bridge here, not a
  protocol translator - see section 3 below.
- **Tier 4 (URTC Tool Head, + optional expansion board over local I2C):**
  CONFIRMED, fully implemented - see the sibling `URTC` repo. Out of scope
  for this document beyond how it's *reached*.

---

## 2. Tier 2 addressing: which of 8 Robot Controller Boards is a frame for?

**PROPOSED** - not yet implemented on the STM32H745 or any Robot Controller
Board firmware.

FDCAN1 "STACK A" is one shared bus carrying up to 8 Robot Controller Boards.
URTC's own protocol (`CANBUS.TXT`) assumes exactly one board per CAN segment,
so its ID blocks (`0x0xx`-`0x2xx` runtime, `0x7Fx` bootloader) are fixed,
un-addressed constants. STACK A needs a slot dimension URTC's protocol was
never designed to carry. Proposal: give each slot `N` (0-7, corresponding to
A1-A8) its own 32-ID window for Robot-Controller-Board-local traffic (its own
bootloader, its own axis/endstop telemetry - a separate concern from
whatever it relays through to its Tier-3 URTC head):

```text
CAN_ID_STACKA_BASE = 0x600
Slot N window       = 0x600 + (N * 0x20)  ..  0x600 + (N * 0x20) + 0x1F
```

Within each slot's 32-ID window, the **same relative offsets URTC's own
bootloader already uses** are reused verbatim, just re-based per slot instead
of fixed - so the Robot Controller Board's own bootloader state machine can
be lifted almost directly from URTC's proven implementation instead of
designed from scratch:

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
| +0x12..+0x1F | Reserved for future Robot Controller Board features | |

Same anti-bricking discipline as URTC's own bootloader: a firmware image is
only copied into the running slot after a full CRC32 + HMAC-SHA256 verify
against a backup/staging slot, so an interrupted or corrupted CAN-OTA update
leaves the previously-working firmware intact.

---

## 3. Tier 3: reaching the URTC Tool Head through the Robot Controller Board

**PROPOSED**, modeled directly on a pattern URTC's own firmware already
implements one level down: its expansion-slave I2C bridge (`CANBUS.TXT` IDs
`0x210`-`0x221`), which relays bootloader and register traffic to a second
MCU it has no direct CAN access to, unmodified and un-reinterpreted.

The Robot Controller Board does the exact same thing one hop earlier: a
dedicated relay window inside its own slot (proposed at slot-relative offset
`+0x20`..`+0x3F`, i.e. `CAN_ID_STACKA_BASE + N*0x40 + 0x20` onward, doubling
each slot's window to 64 IDs to make room) is a byte-for-byte passthrough
onto the board's own second CAN controller, addressed to the URTC head using
**URTC's real, existing, unmodified IDs** (`0x7F0`-`0x7FF` for firmware,
`0x000`-`0x2FF` for the runtime tool protocol). The Robot Controller Board
does not need to understand URTC's protocol at all to relay it - same
"generic passthrough, no interpretation" philosophy URTC's own SPI passthrough
to its expansion driver (`0x180`/`0x181`) already establishes.

Net effect for HYDRA-UMC-STUDIO's Flasher/Tester: flashing or testing a URTC
head is the *same* URTC protocol as if it were connected directly, wrapped in
two levels of addressing (which controller -> which of 8 robot slots ->
relay-to-head) instead of zero. The UI should make this hop count visible to
the operator (see `docs/CAN_OTA_UI_NOTES.md` in HYDRA-UMC-STUDIO if present,
or that app's own Help menu) rather than hide it, since a stall at any one of
the three tiers needs to be diagnosable independently.

---

## 4. What already exists vs. what this fills in

| Piece | Status |
|---|---|
| CM5 <-> STM32H745 SPI transport | CONFIRMED, documented (README.md §10) |
| STM32H745 <-> Robot Controller Board FDCAN1 bus (electrical) | CONFIRMED, documented (README.md §6) |
| Robot Controller Board slot addressing on that shared bus | PROPOSED (this document, §2) |
| Robot Controller Board's own MCU identity | **CONFIRMED: STM32G474RET6** (LQFP-64, 512 KB flash, 3x FDCAN - 2 used, see §1). |
| Robot Controller Board's own bootloader firmware | Does not exist yet. §2's ID map is meant to make writing it straightforward, not a substitute for writing it. |
| Robot Controller Board -> URTC Tool Head CAN link (electrical) | Described by the project owner; not yet on a schematic in `hardware/` |
| Robot Controller Board relay-to-URTC-head behavior | PROPOSED (this document, §3) |
| URTC Tool Head firmware + protocol | CONFIRMED, fully implemented - see the `URTC` repo, `docs/CANBUS.TXT` |
| HYDRA-UMC-STUDIO Flasher/Tester UI | Implemented against a **simulated (mock)** transport that follows this document's addressing scheme - see that repo's own README for current status. Real Tier-1 SPI transport is not implemented (no CM5<->STM32H745 firmware exists yet to talk to).

---

## 5. Superseded documents

`docs/HYDRA-UMC_TECHNICAL.txt`, `docs/HYDRA-UMC_BOM.txt`, and
`docs/HYDRA-UMC_PINOUT.txt` describe an earlier board revision
(STM32H757BIT6/LQFP-208, onboard TMC5160A drivers wired directly to robots,
ESP32-C3, USB hub, Ethernet PHY) that this document's tiered
STM32H745/Robot-Controller-Board architecture replaced. They're marked
superseded at the top of each file and kept for historical reference only -
do not use them as a source of truth for new work.
