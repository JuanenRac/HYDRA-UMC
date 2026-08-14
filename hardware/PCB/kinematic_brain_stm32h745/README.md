# Kinematic Brain — PCB (STM32H745ZIT6 mainboard)

**Status: 🚧 no schematic exists yet.** This is the main HYDRA-UMC
motherboard described throughout the repo-root `README.md` (STM32H745,
Compute Module 5 mezzanine connectors, dual GL3523 USB hubs, M.2 Hailo-8
socket, FDCAN1 STACK A bus, SPI FRAM, motion/actuation stage, power
distribution — sections 1-11) — this folder is where its Eagle project
belongs once schematic work actually starts. Nothing here is a real design
file yet; see `../robot_controller_board_stm32g474/README.md` for the same
status note applied to the *other* board this repo now also scaffolds.

## Starting parts list (BOM.TXT)

Unlike the Robot Controller Board's own BOM (still mostly TBD), this one is
derived directly from the repo-root README's own already-written component
specs (sections 2-11) — a real, reasonably complete starting BOM, not a
guess. See `BOM.TXT` in this folder.

## Once a real schematic exists

Same convention as the Robot Controller Board's own folder: `NETLIST.TXT`,
`PARLIST.TXT`, `PINLIST.TXT` populated from Eagle's own export tooling, plus
a `datasheet/` folder for any board-specific part not already covered by
`../../../docs/datasheets/` (which already has the STM32H745, GL3523,
Hailo-8 family, CM5, and several other relevant datasheets — check there
before adding a duplicate).

**Superseded reference, do not use as a source of truth:**
`../../../docs/HYDRA-UMC_BOM.txt` and `../../../docs/HYDRA-UMC_PINOUT.txt`
describe an EARLIER, different board revision (STM32H757BIT6/LQFP-208,
onboard TMC5160A drivers) — see those files' own superseded banners and
`../../../docs/architecture.md` section 6.
