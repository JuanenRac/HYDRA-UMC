# Slot scan on the stack bus: who is in each slot, and are two boards set to the same one

Status: **contract and host reference only.** No firmware answers the probe yet and nothing in this page has run on a board.
`docs/bus_contract.json` (section `slot_scan`) holds the numbers, `tools/slot_scan.py` is the reference of the frames and of the
verdict, `tests/slot_scan_vectors.json` holds the frames and the scenarios that the firmware has to reproduce, and
`tests/test_slot_scan.py` keeps all of them consistent.

## The problem

The board identifier of a controller board is a 3-bit DIP switch, so nothing prevents two boards from being set to the same slot.
Both then answer on the same identifiers with different data. On CAN that ends in error frames and no readable answer, and an update
sent to that slot would reach both boards or the wrong one.

## Messages (inside the window of a slot)

| Offset | Name | Emitter | Meaning |
|---|---|---|---|
| `0x1E` | `SLOT_SCAN_PROBE` | host (through the kinematic brain) | asks the boards of one slot window to identify themselves |
| `0x1F` | `SLOT_SCAN_REPLY` | bootloader, or the relay layer of the application | the board says who it is |

Both are classic frames of 8 bytes, sent as `slot_base + slot * 32 + offset` (`tools/can_ids.py`).

Probe: `0x5C`, scan id, four zero bytes, complement of `0x5C`, complement of the scan id.
Reply: `0x5D`, board id (low nibble) and role (high nibble, 0 bootloader, 1 application), scan id, 32-bit unique identifier of the
chip (little endian), XOR of the first seven bytes. Anything that does not decode is ignored.

## How two boards of one slot are told apart

A board does not answer at once: it waits `((uid32 * 2654435761) mod 2^32) mod reply_steps` steps of `reply_step` milliseconds, taken
from its own unique identifier, so two boards of the same slot answer one after the other and the host reads two different unique
identifiers in one window. When the delays coincide the frames collide; the host then sees error frames and at most one readable
answer, which is also reported as a duplicate.

## Verdict of one slot (`tools/slot_scan.py`)

| Verdict | What was heard | Update may write |
|---|---|---|
| `ABSENT` | nothing | no |
| `OK` | one board (one unique identifier, however often it repeated) whose board id is the slot | yes |
| `DUPLICATE` | two unique identifiers, or error frames next to a readable answer | no |
| `MISPLACED` | one board whose board id is not the slot of the window | no |
| `NOISY` | error frames and no readable answer | no |

Answers of an older scan (another scan id) are ignored, so a late frame never counts.

## Timing

| Parameter | Value | Meaning |
|---|---|---|
| `scan_window` | `100` | milliseconds the host listens after the probe of each slot |
| `reply_step` | `1` | milliseconds of one delay step (more than two worst-case frames) |
| `reply_steps` | `32` | number of delay steps |
| `probe_repeat` | `1` | probes per slot and scan |

They are proposals to confirm on the bench (`docs/BENCH_TESTS.md` row 15) and move to the configuration file of the machine when
the firmware reads them.

## Pending

- The firmware does not answer the probe, and the host client does not send it yet: implementing both needs the firmware build and a
  board.
- The delay needs the unique identifier of the chip (96 bits on both families; the reference uses 32 of them); the read of that
  register is part of the firmware work.
- Bench: `docs/BENCH_TESTS.md` row 15 (two boards with the same board identifier).

See also `docs/bus_contract.json`, `tools/can_ids.py`, `tools/slot_scan.py`, `tests/slot_scan_vectors.json` and `tests/test_slot_scan.py`.
