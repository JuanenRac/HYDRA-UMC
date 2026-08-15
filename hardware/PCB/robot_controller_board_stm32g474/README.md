# Robot Controller Board — PCB (STM32G474RET6)

**Status: 🚧 no schematic exists yet.** Nothing in this folder is a real
Eagle design today — same honest status URTC's own hardware README
declares for its own board. What's here is the pre-layout scaffolding this
board needs before opening Eagle makes sense: a starting parts list and the
convention this project uses for the text-based BOM/netlist/pinlist exports
Eagle itself can generate once a real schematic exists.

See `../../../docs/architecture.md` for this board's role (Tier 1 — one per
robot, up to 8 per HYDRA-UMC controller) and `../../../src/mcu_stm32g474/`
for the firmware side.

## Starting parts list (BOM.TXT)

Derived from what's already confirmed (the MCU itself, per this project's
own `docs/architecture.md`) plus what a board doing this job structurally
needs (2x CAN transceivers, 6-axis STEP/DIR/EN driver interfaces, endstop
inputs) — NOT a finished BOM. See `BOM.TXT` in this folder.

## Once a real schematic exists

Populate these the same way URTC's own `PCB/` folder does — typically
exported from Eagle via a BOM/netlist ULP script, not hand-maintained:

- `NETLIST.TXT` — net-by-net connectivity, Eagle's own `.cmd`/netlist export format
- `PARLIST.TXT` — part-by-part placement/value list
- `PINLIST.TXT` — MCU pin-by-pin assignment, mirroring the level of detail
  `docs/HYDRA-UMC_PINOUT.txt` gives for the (superseded) earlier board
  revision — write a current one once real net names exist to pull from
- `datasheet/` — datasheets for parts used on THIS board specifically. The
  MCU's own datasheet is already at
  `../../../docs/datasheets/STM32G474RET6.pdf` (added manually — ST's own
  site blocked a scripted download) — only add board-specific parts here,
  e.g. the CAN transceiver once one's chosen
