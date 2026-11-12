# The user-interface stack for the STM32H757

The choice of the library that draws the interface of the H757 (`docs/DSI_EN_H757.md`), with the reasons, and the budget it has to fit. Everything below is a design: **nothing was built or measured on a board**. The figures of the libraries are those their projects state, taken from their public pages; they must be checked again when the library is pinned, and a figure of this design that a measurement replaces is marked as an estimate in `src/ui_h757/budget.json`.

## What the stack has to do here

| Requirement | Why |
|---|---|
| Runs on a Cortex-M7 at 400 MHz with LTDC and the DSI host, framebuffers in external SDRAM | the board (`docs/H757_MEMORY_MAP.md`) |
| A license compatible with this repository (GPL-3.0-or-later) and with a firmware that is public | the repository is public |
| Screens built from **data** (`src/ui_h757/spec`), not drawn by hand in a designer | the same screens are shared with the Flutter app and checked by tests |
| Eight languages, two of them CJK | the rule of the project |
| A build that runs on the PC | the reference rendering and the interface tests run in Actions, not on a board |
| Touch, large targets, text and colour together | a panel beside a machine |

## The candidates

| | LVGL | TouchGFX | Others |
|---|---|---|---|
| License | MIT (stated by the project) | free of charge for STM32, proprietary, closed tooling (ST's terms; check before relying) | emWin: commercial (ST license for STM32 parts); Qt for MCUs and Embedded Wizard: commercial; GUIX: permissive but built around its RTOS; Slint: GPL, royalty-free or commercial terms |
| Fits a GPL-3.0-or-later public repository | yes | the generated code and the tool are not open; a restricted license on the MCU family | depends |
| Screens from a data file | the C API builds widgets at run time: a generator or an interpreter of `ui_screens.h` is direct | screens are made in the Designer and generated as code: our data file would have to be converted into its project format | emWin, GUIX: code |
| Runs on the PC | yes, SDL / X11 / Wayland drivers (stated by the project); the same widget code | a simulator exists but it is tied to the Designer project | varies |
| LTDC, DMA2D | an LTDC driver is listed by the project; the DMA2D draw unit is a configuration of the port | the strongest integration with ST's peripherals | varies |
| Fonts for 8 languages | `lv_font_conv` and FreeType: bitmap fonts made from exactly the characters used | Designer text tools | varies |
| Memory (as stated) | "needs only 32 kB RAM and 128 kB flash" at the bare minimum, "~100 kB RAM, ~200-300 kB flash" for a typical interface, and a render buffer of at least 1/10 of the screen | comparable class | |

## Recommendation: LVGL

1. **The data file decides.** The point of this design is one description of the screens for the firmware and for the app, with tests. LVGL builds widgets from code; a generator can turn `ui_screens.h` into them and a host test can build the same widgets on the PC. TouchGFX would need a conversion into its own project format and would leave the Designer as the real source.
2. **License.** MIT is compatible with the GPL-3.0-or-later of the repository; the closed parts and the restricted terms of the other candidates are not.
3. **The same code on the PC.** The simulator drivers let the interface tests and the reference pictures run in Actions with no board.
4. **The cost is real**: flash. The library alone is the largest block, and the current CM7 application slot does not hold it (below). TouchGFX would not remove that cost; it would only trade it for tool lock-in.
5. **What would change the choice**: a measured size in the CI above the budget, a heap behaviour that cannot be bounded, or a limit of the LTDC/DMA2D port found on the board. The view-model (`src/ui_h757/core`) has no dependency on any library, so the choice can be reversed without touching the rules that keep the machine safe.

The firmware part does not exist yet: the library is pinned by commit when the driver phase starts, and the phase is built in Actions on a temporary branch behind the switch `HYDRA_UI_H757` (off by default).

## Budget against the memory map

Computed by `python tools/ui_budget.py` from `docs/platform_profiles.json` (profile `h757_target`), the texts and the screens, and `src/ui_h757/budget.json`; `tests/test_ui_budget.py` pins the verdicts. Origin of each figure: *data* is counted from the files, *published* is the figure of the library's project, *estimate* is replaced by a measurement.

| Item | Figure | Origin |
|---|---|---|
| Texts in 8 languages (table with pointers) | 19.1 KiB | data |
| Screens, widgets and signals (tables) | 1.2 KiB | data |
| View-model `ui_core` for the Cortex-M7 | limit 12 KiB flash, 256 bytes RAM; built with `arm-none-eabi-gcc -Os` for the Cortex-M7 it is 1560 bytes of flash and no static RAM (the test prints the size on every run) | limit; size measured by compiling, not on a board |
| LVGL flash | 300 KiB | published (upper figure of 200-300 kB) |
| Application reserve of the CM7 besides the interface | 128 KiB | estimate |
| **Flash needed in the CM7 application slot** | **460 KiB** | |
| Slot today (`FLASH_APP_CM7`) | 384 KiB | profile: **does not fit** |
| Slot proposed (rest of bank 1, to `0x08100000`) | 768 KiB | fits with 10 % free |
| LVGL RAM + render buffer (1/10 of the frame) + core + stack | 298 KiB of the 512 KiB AXI SRAM | published + estimate: fits |
| Framebuffers, two of 1280×720 RGB565 | 3600 KiB of the SDRAM | data |
| Glyphs: 78 Latin, 167 Japanese, 153 Chinese (272 distinct CJK) | fonts 243 KiB at 24 and 32 px, 4 bit, uncompressed upper bound | data + estimate |
| Fonts and images in the SPI5 NOR, copied to the SDRAM at start | 755 KiB of 16 MiB | estimate |
| Scan-out at 30 Hz, RGB565 | 55 MB/s against 200 MB/s sustained (the SDRAM bus peaks at 400 MB/s: 32 bits at 100 MHz) | estimate: 28 %, limit 40 % |
| Scan-out at 60 Hz | 110 MB/s = 55 % | **does not fit** the 40 % limit |

What this says:

* **The interface does not fit the CM7 application slot as the profile has it today** (384 KiB). The profile leaves 384 KiB of bank 1 unassigned after the slot; extending the slot to 768 KiB makes it fit with the margin. That change belongs to the owner of the platform profile; until it is made the interface is not built into the application.
* **The fonts cannot be in the internal flash**: the CJK glyphs alone are about 243 KiB. They go to the SPI5 NOR (16 MiB, not memory mapped) and are copied to the SDRAM after the hash is checked; this adds a start-up cost that has to be measured.
* **60 Hz does not fit** the bandwidth rule, which agrees with the board documents (60 Hz does not enter two lanes with margin; 30 Hz is an experimental candidate). The refresh the panel really accepts is a bench question.
* The panel is portrait natively and the interface is landscape: the rotation costs bandwidth or CPU and is not in the figures above.

## Heap and failure behaviour

LVGL allocates; on a machine that must keep stopping, the heap is a fixed pool inside the AXI SRAM and its exhaustion must degrade the drawing, never the stop path. The interface task has its own stack and priority below the stack-bus task and the stop input (`docs/DSI_EN_H757.md`). The proof is a host test with a heap pool that fails on purpose, and on the board a trace of the stop latency with the drawing at its heaviest; neither exists yet.
