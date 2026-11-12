# Changelog: HYDRA-UMC Core 🦾

All notable changes to the hardware and core firmware will be documented in this file.

## [Unreleased]

- **The Jetson link of the display interface and the operator PIN store, in simulation (`src/ui_h757/spec/jetson_link.json`, `tools/ui_link.py`, `tools/ui_pin.py`):** the frames the H757 (SPI master) and the Jetson exchange are data with the framing of the boot exchange and types that do not collide with it: a status that is the heartbeat of the view-model and a still camera picture sent in chunks that the master asks for by offset, with a reference of both sides and a Jetson double that answers, stays silent, corrupts or hangs in the middle of a picture (the signals of the Jetson in the screen data now point at these frames, no longer at pending contracts); the PIN is kept as a salted PBKDF2-HMAC-SHA256 hash in two slots of the F-RAM with a CRC and a sequence number, the failure counter is written before the comparison and survives a restart, a torn write leaves the old record and an unreadable record never lets a PIN in, with a pad that reports only right or wrong to the view-model. Nothing here ran on a Jetson or a board.

- **The interface of the display follows the decisions of the owner (`docs/DSI_EN_H757.md`, `docs/UI_STACK_H757.md`, `config/ui.cfg`):** the CM7 application slot of the H757 profile is 768 KiB (the rest of bank 1) and the interface fits it with 10 % free; an optional operator PIN (`operator_pin_enabled`, off by default, STOP never asks for it) locks every other command with a lock-out and an idle lock, in the Python model and its C twin, replayed on four new scenarios and held by a fuzz; the camera screen shows a still thumbnail and the budget counts its buffers (273 KiB of the SDRAM) and its 4 % of the SPI link; the panel is landscape only; the size of the external SDRAM is a value of the configuration file, marked as not confirmed until the owner confirms the model and the size. Nothing ran on a display or a board.

- **The clock tree of the STM32H757 board is in the platform header and checked when it compiles (`src/mcu_stm32h757/Common/clock_tree.h`, `tests/test_clock_tree.c`):** the generated `platform_profile.h` carries, for the H757 platform only, the 25 MHz crystal, the divider and multiplier of the three PLLs, the divider of each output and the clocks that result (400 MHz core, 200 MHz bus, 100 MHz kernel clock of the FDCAN, the SPI and the I2C4). `clock_tree.h` is free of the HAL and refuses to compile when the numbers do not add up: a PLL output that is not what the table says, a reference or a VCO out of range for PLL1 and PLL2, an FDCAN kernel clock that is not PLL2_Q, or a stack-bus timing that does not give 1 Mbit/s at that clock. The host test recomputes the same numbers and the owners of SPI1, FDCAN1 and I2C4 (the CM7 on this platform); it is registered in `tools/host_tests.py` and `build_firmware.sh`, and 5 Python tests tie the macros to the profile. The code that writes the clock registers of the CM7 is the next step, and the default platform stays the H745 one until the drivers move. Nothing here has run on a board.

- **The bit timing of the stack bus of the CM4 sources comes from the platform profile (`platform_profile.h`), no longer from numbers written in two places:** the FDCAN set-up of the bootloader and of the application takes the prescaler, the jump width and the two time segments from `HYDRA_STACK_BUS_*`. The default platform keeps the numbers the sources had (so the compiled firmware does not change), and building with `HYDRA_PLATFORM_H757` gives prescaler 5 at the 100 MHz kernel clock of the board (1 Mbit/s, sample point 85 %). The tests read the header for both platforms: every init site agrees, each platform gives 1 Mbit/s at its own clock, and the contract tests follow the macros. The move of the SPI1, FDCAN1 and I2C4 drivers to the CM7 and the clock tree of the board are still to do. Nothing here has run on a board.

- **Translations read again:** the new platform text reads naturally in Chinese and Portuguese (technical term for a hung machine, one gender for the Jetson, punctuation).
- **The manual firmware build can check a branch before it is merged, as a dry run with the dev trust table:** `firmware-build.yml` refused every commit that was not in main, so a broken change of the firmware reached main before it was compiled. Now a dry run (`dry_run` on, which makes no draft pre-release, signs nothing, holds no secret and uploads only an unsigned artifact) with the `dev` table may build a commit that is not in main; the Job Summary says so. A draft pre-release (`dry_run` off) and the official trust table (`flavour` release) still need a commit that is in main. Unchanged: only the owner starts it, the ref is checked for odd characters, the commit is pinned in the plan and checked out again by its SHA, and the build is unsigned. The risk is none for production: a branch build never produces a signed artifact or a release. 8 tests read the workflow text: who may start it, the default and the draft, the single way past the check for a commit that is not in main, the pin by SHA, the ref check, no secret and the rule written at the top.

- **The frames of the SPI boot master are a header-only C module, tested on the host against the vectors of the Python model (`src/mcu_stm32h757/Common/spi_boot_master.h`, `tests/test_spi_boot_master.c`):** the bootloader as SPI master of the link to the host (`docs/SPI_BOOT_MASTER.md`) now has its frame code in C: the CRC-32, `Sbm_Encode` and `Sbm_Decode` (the CRC is checked before anything else is believed, then the sync byte, the length and the type of the direction), the HELLO and GET_PAGE builders, the OFFER parser and `Sbm_JudgeOffer`, which refuses a size that does not fit, a target that is not this board and a minimum bootloader newer than this one before a byte is fetched. It builds and reads frames and judges an offer; it never decides that an image is valid, which stays with the verification of the bootloader. `tools/gen_spi_boot_vectors.py` writes the vectors (`tests/spi_boot_master_vectors.json` and `.h`) from `tools/spi_boot_master.py`, `--check` fails when they are stale, and the C test is registered in `tools/host_tests.py` and `build_firmware.sh`: a good frame of every type, every kind of bad frame, every single bit of an offer flipped, the longest payload, the HELLO and GET_PAGE builders and 14 offers with their verdicts. 8 Python tests keep the header, the vectors, the constants and the registration together. The SPI1 driver that moves the bytes and the switch `HYDRA_BOOT_SPI_MASTER` are not part of this module. Nothing here has run on a board.

- **The firmware folder of the Kinematic Brain is `src/mcu_stm32h757`, and the sources build for the STM32H757 device:** the folder `src/mcu_stm32h745` was renamed (history kept with `git mv`), the linker scripts are `STM32H757XIHx_*.ld` and the main files `STM32H757XI_*_main.c`, the builds define `STM32H757xx` and use `startup_stm32h757xx.s`, and every path in the build scripts, the workflows, the tools, the tests, the manifests and the file tree follows. The memory regions and the numbers of the code are unchanged (the same family of dual-core parts), so this is a rename and a change of device header, nothing else: the FDCAN bit timing still assumes the 16 MHz kernel clock of the earlier bench, which `docs/platform_profiles.json` records as `h745_current` until the phase of the clock tree sets the 25 MHz crystal and the 100 MHz PLL2 of the board. The earlier changelog entries keep the old path, as they described it then. Nothing here has run on a board.
- **The interface of the STM32H757 display, as data, a view-model and a budget (`docs/DSI_EN_H757.md`, `docs/UI_STACK_H757.md`, `src/ui_h757/`):** the screens and the texts in eight languages are closed-schema data (`src/ui_h757/spec`, checked by `tools/ui_spec.py`: touch targets, contrast, every signal tied to the real contract or to a contract declared pending, STOP on every screen and never gated, every command refused under listen-only); the view-model (`tools/ui_model.py` and its C twin `src/ui_h757/core/ui_core.c`) decides the mode (starting, normal, degraded, alarm, stopped), what each touch may do and why it is refused, and is replayed on 16 failure scenarios (the Jetson hangs, the SPI link is cut, the bus fills with errors, STOP while moving, listen-only, the two degraded switches in `config/ui.cfg`, both off) in Python, in C with the sanitizers and for the Cortex-M7, plus a fuzz that holds "STOP is never refused"; `tools/ui_gen.py` writes the C tables; `tools/ui_budget.py` computes the flash, RAM, SDRAM and scan-out budget against the memory map and finds that the interface does not fit the 384 KiB application slot of the CM7 as the profile has it today (a 768 KiB slot would) and that 60 Hz does not fit the scan-out; `tools/ui_render.py` draws every screen in every language so that a text that does not fit is found; LVGL is the recommended library. A workflow runs all of it in Actions. Nothing ran on a display or a board; the firmware that draws is a later phase behind a switch that ships off.


- **README: the display on the real-time controller:** the block diagram and a new bullet say that, in the target design, the DSI touch display hangs from the STM32H757, which is meant to run the complete interface so that the H757 and the display keep working (stop, status, control) if the Jetson hangs. It is a design: nothing of it is built or validated.
- **The memory map of the H757 is confirmed only with the datasheet and the documents of the board (decision of the owner):** the datasheet of the STM32H757XIH6 confirms the sizes (2 Mbytes of flash in two banks of 1 Mbyte with eight sectors of 128 Kbytes, 192 Kbytes of TCM, the AXI SRAM of 512 Kbytes, SRAM1 to SRAM3 of 288 Kbytes in the D2 domain, SRAM4 of 64 Kbytes in the D3 domain and 4 Kbytes of backup SRAM) and gives no base address, so every internal region is marked `size_confirmed_by_datasheet_address_to_confirm` in `docs/platform_profiles.json` (`datasheet_confirmation` lists what the datasheet says and what it does not) and in `docs/H757_MEMORY_MAP.md`. The reference manual is not used.

- **The platform header the firmware will read is generated from the profiles (`tools/gen_platform_header.py`, `src/mcu_stm32h745/Common/platform_profile.h`):** for each platform the SPI role of the link to the host, the owner of each peripheral, the bit timing of the stack bus (1 Mbit/s) and the regions of the memory map. `HYDRA_PLATFORM_H745` is the default, because that is what the sources build for today, and `HYDRA_PLATFORM_H757` selects the board of production; `HYDRA_SPI_ROLE` may be set on the command line and otherwise follows the platform (slave on the H745, master on the H757); two platforms or a wrong role do not compile. `--check` fails when the header is not what the profiles say. 7 tests parse the header and compare it with the profiles. No source includes it yet, so no firmware changes. Nothing here has run on a board.

- **README: memory block and file tree:** the diagram shows the external F-RAM on the I2C4 interface of the STM32H757 (not an SPI2 FRAM of 64 KB) and the tree says that the OS image folder is the CM5 pilot bench today, with the Jetson image still to be defined.
- **The bootloader as SPI master, in a host model (`tools/spi_boot_master.py`, `docs/SPI_BOOT_MASTER.md`):** the board of production is the SPI master towards the Jetson, so the bootloader of the application core asks the host for a signed image and receives it page by page (frames with a sync byte, a type, a sequence number, a length and a CRC-32; the READY line is raised by the host when it has something to ask for). The mode is off by default (`HYDRA_BOOT_SPI_MASTER`, `BootConfig.spi_master_enabled`): with it off the link is not touched and the update goes on over CAN. The chain of trust is the one of the CAN update, whole: the image goes through `image_header.verify_image` (signature first, then the CRC, the hardware identifier, the link address, the vectors, the stack in RAM and the anti-rollback rule with its signed downgrade token for the pair of versions and the nonce of the session) and only then is the staging slot valid; a size that does not fit, a target that is not this board and a minimum bootloader newer than this one are refused before a byte is fetched. 25 tests: a changed byte anywhere, another key, hardware, link address, stack, equal or older versions and wrong tokens are refused; a damaged page is asked again and one that stays damaged, a READY line that never rises or drops halfway, an answer for another request and a shorter image end the attempt with nothing staged; a fuzz of the link with a fixed seed never leaves an unverified image valid. Nothing here has run on a board or a Jetson.

- **The memory map of the H757 board is data, with the document each region comes from (`docs/H757_MEMORY_MAP.md`):** `docs/platform_profiles.json` has a `memory_map` for the profile of today (equal to the linker scripts, checked by a test) and for the H757 (the internal layout inherited and marked to confirm against RM0399, the two 128 MiB SDRAM banks at `0xC0000000` and `0xD0000000` from the board documents, and the contradictions with the H745 repository: no external memory there, the NOR flash is on SPI5 and not QSPI, the CM7 owns the peripherals). `tools/platform_profile.py` generates the MEMORY block of a linker from the profile (`linker_memory`) and checks overlaps and sources (`memory_problems`). 6 new tests. The RAM and flash of the H757 are not decided here by default: they are the family values, to be confirmed against the reference manual before a linker uses them.

- **README: the block diagram and the feature list draw the Jetson baseboard:** the title, the neural-coprocessing bullet and the diagram no longer show the Compute Module 5 carrier with its PCIe switch and Hailo-10; they show the Jetson Orin NX 16 GB host with a Hailo-8, an NVMe drive and a Wi-Fi/Bluetooth module, USB 3.0 camera hubs and the SPI link to the STM32H757, and a note says that the platform profiles are meant to be configurable. It is a design: no board has been built.
- **The controller board and its two buses are data, and the tool head bus is 1 Mbit/s:** `docs/platform_profiles.json` has a `controller_board` section (STM32G474: the timing of FDCAN1 on STACK A and of FDCAN2 on the downlink to the tool head, both 1 Mbit/s with prescaler 1 and segments 13 and 2 at the 16 MHz kernel clock, a sample point of 87.5 %), `tools/platform_profile.py --check` verifies them against `docs/bus_contract.json` (rate error 0, sample point from 75 to 90 %) and 4 new tests compare them with what `RobotControllerRelay.c` sets for both handles. The CAN contract copy follows the decision of the owner: the tool head bus is 1 Mbit/s (`TOOL_HEAD_BIT_RATE_BPS`) and the 500 kbit/s of the earlier URTC protocol is only the documented legacy value (`TOOL_HEAD_LEGACY_BIT_RATE_BPS`). The head side (the URTC spec, the head firmware, their bootloaders) moves in the URTC repositories. Nothing here has run on a board.

- **A corrupted `config/machine.cfg` is refused with a reason, never with an exception of the parser:** `tools/machine_cfg.py` let the errors of `configparser` and of the text decoding (a file with no section, bytes that are not UTF-8) escape, so a build or a check that only knows `ConfigError` would have died with a traceback instead of saying the file cannot be used. The loader now turns them into `ConfigError`. `tests/test_machine_cfg_corruption.py` (3 tests, a fixed seed, 600 mutations of the real file: a flipped byte, a cut file, a missing or repeated line, a huge or non-numeric value, control characters, bytes that are not UTF-8, unknown sections) checks the two allowed endings: a refusal, or a configuration in which every stop category is 0 or 1 and the header can be generated. The test found the bug above on its first run. Nothing here has run on a board.

- **The CAN contract copy now carries the stop frame and the tool head identifiers:** `docs/can_ids.json` (the `CanIds` contract of the SDK) has the 8-byte frame of the emergency stop (`estop_frame`: bytes, kinds, reasons, sources) and the 106 messages of the URTC tool head bus (`tool_head`), and `tools/can_ids.py` exposes them as `ESTOP_FRAME`, `TOOL_HEAD_MESSAGES` and `TOOL_HEAD_BIT_RATE_BPS`. 4 new tests: the stop frame of the contract is what `tools/estop_can.py` implements (kinds, reasons, sources, identifier) and agrees with `docs/bus_contract.json`, the tool head protocol starts at 0x095 above the stop and the stop is not one of its identifiers, and the two bit rates the repository gives to the tool head bus are both kept in view: the URTC protocol is 500 kbit/s and the downlink of the controller board is written for 1 Mbit/s, a disagreement the contract records and does not decide. Nothing here has run on a board.

- **The platforms the brain can be built for are data (`docs/platform_profiles.json`, `tools/platform_profile.py`):** two profiles, `h745_current` (what the sources are built for today, CM5 as SPI master of a slave board, 16 MHz FDCAN kernel clock) and `h757_target` (the board of the CubeMX project: STM32H757, NVIDIA Jetson Orin NX as host with the board as SPI master, CM7 owner of SPI1, FDCAN1 and I2C4, the clock tree of the project, the pins of the stack CAN, the host SPI, the F-RAM bus and the peripherals that have no driver yet). The stack bus of the target keeps 1 Mbit/s as the owner decided (prescaler 5 at the 100 MHz kernel clock, a sample point of 85 %; the CubeMX project has prescaler 10, 500 kbit/s, kept in the file as `cubemx_stack_bus`). The F-RAM timing `0x009034B6` is written as a candidate not confirmed on a bench and the firmware still has no default for it. `python tools/platform_profile.py --check` prints each profile and fails when one is inconsistent. 11 tests: the clock tree recomputes from the 25 MHz crystal, every platform gives 1 Mbit/s and a usable sample point, the profile of today is the timing of `KinematicBrainCan.c`, the owners and the roles follow the decisions, and a wrong profile is caught. Nothing here has run on a board and no firmware reads the profile yet.

- **The table of CAN identifiers is read from the contract of the SDK:** `docs/can_ids.json` is the `CanIds` contract of HYDRA-UMC-SDK (the emergency stop identifier, the base, width and number of the slot windows and the 19 messages of a slot with the one node that sends each), and `tools/can_ids.py` builds its `MESSAGES`, `SLOT_WINDOW`, `SLOT_BASE` and `ESTOP_CAN_ID` from it instead of keeping the numbers in Python. The host tools and tests that import the table are unchanged. 4 new tests: the table is the file, the file agrees with `docs/bus_contract.json` (slot base, window, slots, stop identifier), every identifier of every slot fits in 11 bits with the stop below them, and, when the SDK is next to this repository, the copy is the contract of the SDK byte for byte. Nothing here has run on a board.

- **The documentation names the real target of the board:** an STM32H757 (XIH6, TFBGA240) that is the SPI master towards an NVIDIA Jetson Orin NX 16 GB, with the Compute Module 5 kept as the pilot development bench. The README in the eight languages, the architecture (a new section 0 says what is decided, what the code does today and what is open), the bench tests, the build switches, the F-RAM page and the hardware specification say so; the snapshots of earlier revisions are marked as historical, and the files that still describe the STM32H745 development target or the Compute Module 5 carrier are marked as such. The firmware sources and the build target keep their names, and nothing of the master role has been written or run on a board.

- **The SPI bridge publishes the emergency stop as the brain holds it:** `GET /estop` of the local service answers `latched`, `link_ok` and `stop_enabled` read from the telemetry of the kinematic brain, whose latch is what the frame on identifier `0x001` sets (the stop of the safety zones, of a board input or of a person), so the server can show one stop state without a second reading of the bus and without any change in the firmware. A brain that does not answer is `online: false` with the rest empty, a busy link is a 409, and nothing is invented. Tested on the host with a fake brain; not run on a board.

- **The slot window of the stack bus is 64 identifiers wide, and the kinematic brain can erase and self-test its F-RAM over the SPI link (simulated, switch off):** the first 32 identifiers of a window were all taken, so the window is now `slot_base + slot * 64 + offset` (the eight slots fill `0x600` to `0x7FF`), the filter of a controller board is mask `0x7C0` and the one of the brain `0x600`, in the three bootloaders and the application, and `tools/can_ids.py`, `docs/bus_contract.json` and the documents follow; the three firmwares must be flashed together. The chip is the FM24CL64B (8 KiB, I2C) on the main board, on the local bus I2C4 (`PD12` clock, `PD13` data, address `0x50`), so the requests go from the CM5 to the CM4 over the SPI link as the frame types `0x28` and `0x29` of the tier `SELF` and never on a CAN bus; the offsets `0x20` to `0x23` of the window stay reserved and the bootloader of the controller board no longer answers them (`docs/FRAM_ERASE.md`). `src/mcu_stm32h745/CM4/fram_node.h` holds the logic with no HAL (decode, refusals by name, erase, a non-destructive self-test) behind the switch `HYDRA_FRAM_ERASE` (off), and `fram_i2c.h` the driver of the chip with no HAL: two address bytes, chunks without a page limit, three tries per chunk, a read-back of every block, a hook between chunks that keeps the stop frames going, and no access past the end of the chip; the HAL glue in `SpiApp.c` needs `HYDRA_FRAM_I2C_TIMING`, which has no default on purpose, so a build without it answers `no-chip` and a build without the switch answers `disabled`. The CM5 side has `fram_client.py` and the routes `POST /fram/erase?confirm=ERASE` and `POST /fram/selftest` of the SPI bridge, off unless the service is started with `--enable-fram-erase`, and `GET /version` announces the capability. Tested on the host with a simulated chip, a fake I2C bus and a simulated brain; not run on a board (bench rows 30 and 31 are open).

- **The host tests of the firmware are measured module by module, and the format 2 header and its decision run on all three bootloaders:** `tools/host_coverage.py` builds every C host test with gcc coverage, reads the counters with gcov and prints the share of lines that ran for each firmware module on its own (a function that nobody calls shows as 0 %, and a line that one flavour of a test leaves out counts when another flavour runs it); `tools/host_coverage_floor.json` holds a floor for each of 52 modules, 100 % for the ones with a switch (the FDCAN kernel clock, the request to enter the bootloader, the health frame, the adoption guard, the RCC owner, the SPI system layer, the flash layer, the boot decision, the trust table, the slot scan) and the measured value for the rest, and a workflow runs it where a compiler exists. The new `test_format2_decision` builds format 2 headers by hand and checks each refusal of the parser, a sweep with a fixed seed over the bytes of a header, the signature message and every verdict of the decision with a trust table, in the order of the decision, on the copies of the G474, CM4 and CM7 bootloaders (the image header went from 74 % to 100 % and the update decision from 42 % to 100 % on the two H745 cores); the answer of the safe-state flags with the switch off is checked too. `tests/test_host_coverage.py` checks the arithmetic and the reading of the tool.

- **More tests for the signed image header, the trust table, the host test runner, the machine configuration and the changelog extractor:** every refusal of the bootloader decision has its own case (short or reserved bytes, a signature from another key, a wrong CRC, hardware or address, vectors that disagree with the image, a reset vector outside the image, a stack pointer outside RAM, the anti-rollback rule and a downgrade token for the wrong pair or session), the signing and trust command lines run end to end (a key is never overwritten or printed, the headers are written and compared), a seeded sweep requires that no byte changed in a good image or table is ever accepted, and the host test table, its compiler calls (replaced), the machine configuration limits and the changelog section extractor are checked at their edges. Coverage of the image header and trust table tools goes from 67 % and 64 % to 99 %.

- **The flashing side enforces `min_bootloader` and `target` of the update manifest, when the caller passes them:** `POST /flash` of the SPI bridge takes `target` and `min_bootloader` (the fields of the manifest of the same names, both optional; without them nothing is checked, as before). A target that is not the board (`g474`, `h745-cm4`, `h745-cm7`) is a 400 before the image is read and before the board is touched; a bootloader older than the minimum, or one whose version cannot be read, ends the stream with an error after the board entered its bootloader and before a single byte of the image is sent. The version comes from the frame the bootloader sends after its version answer (`query_bootloader_version`). Tested with the fake bootloader transport (the exact minimum, newer, older, four-number versions, a silent board, a wrong target, and over HTTP); the answer of the real bootloader on the bench is still to be seen.

- **A board whose set-up fails a second time says so before it holds still, when its uplink already works:** with `HYDRA_INIT_FAULT_LATCH` and `HYDRA_HEALTH_FRAME` on, a set-up call after the start of FDCAN1 that fails again sends one health frame with the new flag `INIT_SAFE` (bit 6, with `INIT_BAD`) and then goes to the safe state, so the brain can tell "set-up failed, holding still" from a board that simply fell silent; when the uplink itself is what failed nothing can be sent, as before. The flag is in `health_frame.h`, in the Python reference, in the parity test and in the host test, and the build script now compiles the application of the controller board with those switches on.

- **More tests for the tools that keep the bootloaders and the vectors in step:** the generator of the C vector headers (the committed headers are what the JSON gives, a stale or missing header and a changed JSON are noticed, a scenario that does not fit the header is refused, the expected verdicts are those of the Python reference), the four commands of the shared-bootloader tool on fake chip directories (import takes only what is identical in all three, a drifted or missing copy is told and repaired, status counts the lines that differ) and the size gate of the bootloaders with a stand-in size tool (margins, a missing or ambiguous ELF, a missing region, the table and the exit code).

- **The application of the CM4 can run the full slot scan when the CM5 asks for it, behind `HYDRA_SPI_SLOT_SCAN` (off by default):** the scan asked for through the SPI link sends the probe to each slot, listens to the replies and the error frames of the controller for the window of the slot, and gives the five verdicts (absent, ok, duplicate, misplaced, noisy) instead of the present-or-absent answer of the AXIS_STATUS query; the stop frames keep being read and sent during the whole scan. The order of the windows and the verdicts that reach the CM5 are tested on the host (`tests/test_slot_scan_sweep.c`, with a stand-in bus that replays the shared scenarios), and the build script now compiles `SpiApp.c` with every switch on to prove that it builds.
- **The page buffer flush, the start of an update, the confirmation of a healthy start and the data frames of the three bootloaders are one copy:** `boot_update_core_impl.h` in `firmware_base/boot` is included by the three `bootloader_protocol.c` after `boot_update_glue.h`, in place of four functions that were identical in the three files (checked function by function before the move). The host tests of the three bootloaders and the build of the three chips run before and after the move.
- **The CRC32 and the checks of a metadata record of the three bootloaders are one copy, and what still differs is written down with its reason:** `boot_crc32_impl.h` and `boot_meta_record_impl.h` in `firmware_base/boot` are included by the three `bootloader_flash.c` where the same text used to be (checked function by function before moving it; the host tests of the three bootloaders and the build of the three chips pass unchanged), and `docs/FIRMWARE_BASE.md` lists, file by file, what is the same in all three, in two, and what differs because of the chip (page or sector, FDCAN or SPI or mailbox, the RAM range of the stack pointer).

- **The full verdict of the slot scan is in C on the master side, and tested on the host:** `slot_scan_master.h` (HAL-free, in the folder of the application of the CM4) builds the probe, reads the replies (length, magic, XOR check, board id and role in range) and gives the verdict of one slot (absent, ok, duplicate, misplaced or noisy); an update may write only on ok, answers of another scan and damaged frames never count, error frames next to one answer are a duplicate, a scan that was not begun allows nothing, and the counters stop at their maximum instead of wrapping. `tests/test_slot_scan_master.c` replays every scenario of the shared vectors (generated into `tests/slot_scan_vectors.h`, the same list the Python reference is tested against), every single-bit damage of a reply, the constants of the board side and a seeded fuzz against an independent model of the rules. Nothing here has run on a board.

- **The bridge of the CM5 sends the safety-zones heartbeat to the brain, and only while the chain is healthy:** with `--zones-state-url` (off by default) `python -m spi_bridge serve` asks the safety-zones service for its verdict every `--zones-period` seconds and sends the `ZONES_HEARTBEAT` command over the SPI link only when the verdict is READY, carries a time with a zone and is not older than `--zones-max-age` seconds (nor from the future). A service that is down, frozen, inhibited, in a safe stop or that answers something that is not a verdict sends no heartbeat, which a brain built with `HYDRA_SPI_ZONES_HEARTBEAT=1` reads as a loss; a busy link skips the pass instead of waiting, and the sender never asks the brain to stop. 19 tests.

- **The Python checks of the repository are written down and run on every push:** a `pyproject.toml` holds the settings of ruff and mypy that the other Python repositories of the family use (the pyflakes errors, the syntax errors and the likely bugs of flake8-bugbear; a missing optional module is not an error), the baseline workflow now runs ruff over `src`, `tests` and `tools` and mypy over `src`, and the few findings that were there are fixed (a loop variable that was not used, a `zip` without `strict`, and the transport of the bridge now says what it needs of the SPI device and of the data-ready line with two protocols instead of `object`).

- **The brain stops the stack when the heartbeat of the safety-zones chain stops coming, behind a switch that ships off:** a new command of the SPI link (`ZONES_HEARTBEAT`, type `0x26`, `ZONE`) is sent by the CM5 only while the last verdict of the safety-zones service is ready and fresh. With `HYDRA_SPI_ZONES_HEARTBEAT=1` the application times it (`HYDRA_SPI_ZONES_MAX_AGE_MS`, 1000 ms by default, with a clock that may wrap) and stops the stack with the reason `safety-zone` when it has come and then stops; it never trips before the first heartbeat, so a machine without that service is not affected, and the next heartbeat arms it again. It needs the stop switch, a wrong magic or length does nothing, and the bridge of the CM5 sends it with `send_zones_heartbeat`. Covered by the C host tests (the lost heartbeat against the real state machine of the brain), the Python twin and the bridge tests. Not run on a board: bench row 13.

- **The shared part of the three STM32 bootloaders has one copy and a check:** `firmware_base/boot` holds the fifteen files that are the same in the controller board, the CM4 and the CM7 bootloaders (the SHA-256, the signature check, the image header, the boot decision, the trust tables, the flash layer and TweetNaCl), and `tools/sync_firmware_base.py` (`status`, `check`, `sync`, `import`) keeps the copies in the chip directories byte for byte equal to it; the tests run `check`. `bootloader_crypto.c` and `.h` differed only in a comment and in the HAL include, which `bootloader_common.h` already brings in, so they were made one file. `status` lists what still differs between the chips and by how many lines (the common header, the flash routines, the protocol and the main), which is the size of the next step. No bootloader logic changed.

- **The application of the CM4 answers the SPI link, behind a switch that ships off:** until now only the bootloader had an SPI slave; once the application started, the CM5 had nobody to talk to. `spi_app.h` (the logic, without any HAL) checks the seal of every frame, drops and counts a damaged one, does not run a retry twice and hands the commands of the tier `SELF` to `spi_system.h`; `SpiApp.c` is the driver (SPI1 in slave mode on the pins of the bootloader and one task) and compiles to an empty unit unless a build sets `HYDRA_SPI_APP=1`. The SPI task never calls the emergency stop: it leaves the stop, release and slot-scan requests for the STACK A task, which publishes the state of the stop and the counters of the bus. `HYDRA_SPI_REQUIRE_CRC` refuses frames without the seal and `HYDRA_SPI_RELEASE_EXPECTED_MASK` names the slots that have to confirm a release (all eight by default, so an empty slot keeps the stack stopped). The scan of the application tells an answering board from a silent one; the full verdicts need the scan of the slots. `tests/test_spi_app.c` runs it against the real state machine of the brain with the switches on and off, `tools/spi_app.py` is its host twin and `tests/test_spi_app.py` compares the bytes of the two. The bridge of the CM5 gained `system_client` (`read_telemetry`, `probe_slots`, `request_stop`, `request_release`) with its tests. Not run on a board: bench row 13.

- **The manual Renode workflow runs the bootloader of the controller board:** `renode-g474.yml` has a second job that loads the committed bootloader ELF into the hand-written STM32G474 platform, runs it for one virtual second and checks that the run finishes with the program counter inside the flash; the clock tree, the flash controller and the FDCAN engine stay unmodelled, so it only shows that the code runs from the reset vector.

- **The three bootloaders can erase, write and copy through the common flash layer, behind a switch that ships off:** with `HYDRA_FLASH_LAYER=1` the routines of `bootloader_flash.c` (G474, CM4 and CM7; the H745 cores keep their bank and erase whole sectors) call `flash_layer.h` (range, alignment, blank-before-write and a read-back of every double-word) and leave only the three hardware calls to the chip; a write over data is now refused before the controller is asked. The host tests of the G474 bootloader (flash and metadata log, and the whole update with its cuts) and of the sector erase of both H745 cores run on and off; the H745 flash-word program cannot run on a 64-bit host, so it is compiled for the chip and checked on the bench.

- **A failed set-up of the application resets once and then stays in a safe state, behind a switch that ships off:** with the new switch `HYDRA_INIT_FAULT_LATCH` (off by default) the first failed set-up call (clock selector, FDCAN init, filter, start) writes a counter into two backup registers and resets the chip once, the second one leaves the driver enables off with the interrupts off and refreshes the watchdog in a loop (no reset circle, no motion), and a start after a failed one reports `INIT_BAD` in its health frame (`init_fault.h`). A counter with its complement means random bits are not a counter. The host tests run it on and off, including a flipped bit at every position.

- **Foreign traffic at the maximum rate and an update cut at every byte are tested without a board:** `tests/test_foreign_traffic_flood.py` floods a controller board model (the window filter of mask `0x7E0`, the exact emergency stop filter and a receive FIFO of the depth in `tests/timing_models.json`) with the other slots, seeded random identifiers and the emergency stop: only its own window and `0x001` get in, the FIFO never overflows, and the same flood overflows it without the filter. `tests/test_bootloader_g474_update_host.c` now cuts the data of an update after every byte (an image of 512 bytes and one that crosses a flash page): the main slot and the installed metadata stay as they were, the end of the update after a short transfer is refused, a new session installs the whole image, and the data fragmented in pieces of 1 to 8 bytes gives the same result. Simulated, not run on a board.

- **The application can report what the bootloader found at power-up:** with the new switch `HYDRA_BOOT_REPORT` (off by default) the bootloader leaves, in two backup registers and just before it jumps, whether the reset was a watchdog reset and whether one of its own set-up calls (clock, FDCAN init, filter, start, SPI) failed, and the application turns them into the `WDG_RST` and `INIT_BAD` flags of the health frame (`boot_report.h`). It is a word with its complement, so random bits after a power-up are not a report, and the report applies once. The host tests run it on and off, including a flipped bit at every position.

- **A hand-written Renode platform of the STM32G474 loads in a manual workflow:** Renode has no platform for the STM32G4 family, so `tools/renode/stm32g474.repl` declares the Cortex-M4F with its NVIC, the flash, the RAM blocks, the FDCAN message RAM, USART1, two SPI units, the watchdog and the GPIO ports, `tools/renode/stm32g474.resc` loads it and `.github/workflows/renode-g474.yml` (started by hand, the same pinned Renode release and hash as the platform check) fails if Renode reports an error while reading it. `docs/RENODE.md` lists what is not modelled (the clock tree, the flash controller registers and the FDCAN protocol engine); `tests/test_renode_g474_platform.py` checks the memory map against the flash geometry of the bootloader. Nothing of the firmware runs in Renode yet.

- **The firmware build takes extra compiler flags, and the switches that ship off were compiled with all of them on:** `HYDRA_EXTRA_CFLAGS` is added to the flags of every chip, so the application and bootloader switches can be compiled in a CI run (the build and the host tests passed with the FDCAN clock on by default and with every other switch at 1). Compiled only; nothing was flashed.

- **After a bus-off the emergency stop node refuses a release until it hears the brain again:** a node that went bus-off stays latched, and until a new `ALIVE` frame of the brain arrives it also refuses a `RELEASE`, even when the last heartbeat before the bus-off is still inside the timeout (`estop_can.h` of the controller board and of the CM4, the Python reference, a new scenario of the shared vectors and a test on the virtual bus). Simulation only: bench row 17 stays open.

- **Build switches that ship off make the firmware ready for the bench rows:** the FDCAN kernel clock can be chosen explicitly (`fdcan_clock.h`, the selector resets to the HSE and no HSE runs), the application of a controller board can answer `ENTER_BOOTLOADER` with the exact magic and only with the drivers off (`app_boot_request.h`: a request word in a backup register, read once by the bootloader), the application can send a health frame once a second on `slot_base + 0x11` (`health_frame.h`, `tools/health_frame.py`), the bootloader can refuse the read-back of the application while the read-out protection is on and read the metadata back after the first-boot adoption (`adoption_guard.h`), the tick interrupt can be kept off until the scheduler starts, and one core of the H745 can be chosen with a hardware semaphore to set the clock tree up (`rcc_owner.h`). Every switch is `0` by default except the FDCAN kernel clock, which the maintainer decided to ship on (it is not validated on a board: bench row 10 is still open, and `-DHYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT=0` restores the old binary), and `docs/FIRMWARE_SWITCHES.md` lists them with the bench row that has to pass before one is turned on.

- **The SPI link has commands for telemetry, probe, stop and release:** `spi_system.h` (frame types from `0x20`, never on a CAN bus) answers a counters query and runs the slot scan before an update, and, with a second switch, asks the brain for an emergency stop or starts the release procedure. `docs/SPI_SYSTEM.md` is the contract, `tools/spi_system.py` the reference and the host client, and `tests/test_spi_system.c` runs the header against the real state machine of the brain. Both switches are off, so the CM4 answers none of these frames until a build asks for them.

- **One flash layer for the three bootloaders:** `flash_layer.h` (the same file in the three boot directories) holds the checks the bootloaders wrote three times (range, alignment, blank before write, read-back after every program unit, a copy that checks every block) with only the three hardware calls left to each chip. `tests/test_flash_layer.c` runs both geometries (pages of 2 KB and double words on the G474, sectors of 128 KB and flash words of 32 bytes on the H745) on a simulated flash with a power cut at every program unit: the source is never touched and a cut is never reported as a success. The bootloaders do not call it yet.

- **A virtual bench runs the multi-board scripts of the bench rows:** `tests/sim_hil_bench.py` has doubles of the controller board and of the brain on the virtual CAN bus, a batch orchestrator that scans first, hands each board over to its bootloader, flashes them one after the other and reports the duplicate, the silent board and the cut flash, the CM5 side of the new SPI commands, and the translation of the safety-zones frame into a stop request of reason safety-zone. `tests/sim_timing_models.py` (numbers in `tests/timing_models.json`) models the frames lost before the scheduler starts, the SPI link at full rate with damaged and repeated frames (a flipped marker is accepted unchecked unless `HYDRA_SPI_REQUIRE_CRC` is 1) and the latency of a status read through the relay. All of it is simulation, and the bench rows stay open.

- **The bus contract runs over a real kernel CAN stack on demand:** the manual workflow `vcan-check.yml` brings up `vcan0` on a runner and `tests/vcan_check.py` sends the emergency stop, the slot scan and the health frame through real sockets and reads them back, together with every identifier of the table in the last slot.

- **The seal of the mailbox between the two cores and the bit timing of every FDCAN set-up are checked on the host:** `tests/test_ipc_mailbox.c` runs twice (the build switch `HYDRA_IPC_REQUIRE_CRC` off and on) and checks that a sealed frame is valid, that every flipped bit of the frame, of the sequence and of the CRC is caught, that a frame without the marker is accepted or refused as the switch says, that the CRC equals the one of the SPI frame, and, with a model of the two cores and the two semaphore channels that uses the real seal functions, that a second command or answer waits instead of overwriting the first, that a damaged frame frees the channel, and that 20000 random interleavings deliver every answer once and in order. `tests/test_fdcan_timing.py` reads the five FDCAN set-up sites of the two chips and fails when they do not use the same timing, and checks that the timing gives 1 Mbit/s at the kernel clock the bootloaders assume; the real clock is still bench row 10. Simulation only: nothing here has run on a board.

- **A manual workflow lists what Renode models of the two firmware chips:** `.github/workflows/renode-check.yml` downloads one fixed release of Renode, checks its SHA-256, and `tools/renode_gap_report.py` writes to the Job Summary which platform descriptions exist for the STM32G4 and STM32H7 families and which of FDCAN, SPI, the hardware semaphore and the flash controller they declare; a chip without a platform is reported as not supported. Nothing of the firmware runs yet: it is the list of gaps to close before a bootloader can start in Renode.

- **A virtual CAN bus runs the slot scan and the emergency stop contract on the host:** `tests/sim_can_bus.py` emulates a bus in one process (arbitration by identifier, a capacity that follows the bit rate, error frames when two nodes send the same identifier with different data, lost frames, a node forced bus-off, and a record of every sender that uses an identifier that is not its own); `tests/test_can_bus_sim.py` scans slots with a board alone, absent, doubled, colliding and misplaced, and runs the emergency stop state machine on it under a saturated bus, with a lost frame, silence of the brain and a bus-off node. Simulation only: the timings are those of the model and the bench rows stay open until they are measured on boards.

- **The flash cycle of the CM5 bridge is tested against a simulated A/B bootloader with a fault on every frame:** `tests/sim_board.py` models a bootloader that stages the image in the inactive slot and switches only after the size and the CRC-32 verified; `tests/test_flash_faults.py` cuts the power at each frame of an update, flips a bit of each frame, loses or repeats each answer, fails a flash page and sends a wrong CRC, and checks that the active image is never a mixture, that nothing reports done for an image that was not installed and that a repeated update afterwards succeeds. Simulation only: the same cases on the boards are not done yet.

## [0.2.8] - An official firmware release is signed on the maintainer's machine and published by a workflow that only verifies: the new workflow .github/workflows/release.yml (started by hand, only from main, behind the release environment...

- **An official firmware release is signed on the maintainer's machine and published by a workflow that only verifies:** the new workflow `.github/workflows/release.yml` (started by hand, only from `main`, behind the `release` environment with a required reviewer, every action pinned by commit, no secret) checks that the tag is the version of the commit, that no published release of it exists and that exactly one draft of the tag holds the signed files, verifies the set with public keys only, creates the release from the verified copy with the section of the CHANGELOG as its notes, verifies the published files again and removes the draft. `tools/publish_release_assets.py` checks the set (the six components against `SHA256SUMS` and the build manifest, the release trust table in every bootloader, the image signature `.hdr2` and `.sig2` of each application image decided the way a bootloader with the release table decides it, the release signature `.sha256` and `.sig` of every file), refuses as official any set built with the public development table and prints the `gh release create ... --draft` command; `tools/verify_release_set.py` repeats the same checks, adds that no file of the folder is unaccounted for, and is what the workflow runs. `tools/release_sign.py` verifies the release signature (the format of `hydra-sign`) against the pinned public key of the releases; `tools/changelog_section.py` prints the notes of a version. Procedure and rules in `docs/RELEASE_SIGNING.md`; tests for each tool and for the workflow use throw-away keys only.

## [0.2.7] - A physical emergency stop input and a stop category per axis, both read from config/machine.cfg: the controller board reads a normally closed loop on a pin chosen in the file, with the pull opposite to the healthy level so a... (and 18 more changes)

- **A physical emergency stop input and a stop category per axis, both read from `config/machine.cfg`:** the controller board reads a normally closed loop on a pin chosen in the file, with the pull opposite to the healthy level so a cut wire or a lost contact is a stop, starts tripped, and debounces a stop faster than the return to healthy. A tripped input latches the board through the same decision as a stop heard on the bus (whichever comes first), sends a stop with reason hardware input on the stack bus and to the tool head, shows as LATCHED in `ESTOP_STATE`, and a RELEASE cannot unlatch the board while the loop is open. The default stop category of every axis is an immediate cut of the driver enable; an axis can be set to decelerate first and is cut after a time limit at the latest. `tools/machine_cfg.py` turns the file into `machine_config.h` (a test fails when it is stale); `production.rdp_level` (0, off) is in the same file for the switch of STUDIO. `tests/test_estop_input.c` and `tests/test_machine_cfg.py` check it on the host; nothing has run on a board. See `docs/ESTOP_CAN.md`.
- **One update flow for the three bootloaders:** the functions that were the same in the bootloaders of the controller board and of the two cores of the kinematic brain (the chunks of the signature, the header and the downgrade token, the session nonce, the reason the host is told when a check fails) now live once in `boot_update_glue.h`, which each `bootloader_protocol.c` includes; only how a frame is sent, the RAM range and the end of a successful update stay per target. A test keeps the three copies of the header identical and fails when a bootloader defines one of these functions a second time.
- **The emergency stop on identifier 0x001 is in the firmware:** `estop_can.h` (the same file for the controller board and the kinematic brain) holds the frame, the rule that anything malformed is a stop and the state machine of a node; `estop_brain.h` holds what the master sends. The controller board starts latched with the driver enables off, forces them off in the same pass in which a stop arrives, passes the frame unchanged to the tool head, latches on a bus-off and on the silence of the master, answers `ESTOP_STATE` and refuses the normal enable command while latched; the brain starts stopped, repeats the stop, announces itself with alive frames, and ends a stop only after a requested release that every expected board confirmed (otherwise it stops again and names the board). `tests/test_estop_can.c` replays the vectors and runs a brain with several nodes on a simulated bus. The SPI command that asks the brain for a stop or a release, the tool head side and the bench tests are still open.
- **The CM5 bridge seals its SPI requests:** `spi_bridge/transport.py` writes the marker, a sequence and the CRC16 into the padding of every frame it sends to the kinematic brain and rejects a sealed answer whose CRC does not match (an answer without the marker is still accepted, and `HYDRA_SPI_REQUIRE_CRC` stays off until the bench row has run). Tests compare the sealed frame with the reference `tools/spi_frame.py`.
- **The controller board answers the slot scan probe:** the bootloader and the application answer a probe in the window of their own slot with the board identifier, the role and a 32-bit fold of the unique identifier of the chip, after a delay taken from that identifier (`slot_scan_node.h`), so two boards set to the same slot answer one after the other. Tested on the host against the vectors of the contract. The host client that sends the probe is still to be written.

- **Nothing compiles on the PC of the developer by accident:** a build that needs a C compiler, Cargo, Go, Gradle or an npm build runs only on a CI runner (`CI` or `GITHUB_ACTIONS` set) or when `ALLOW_LOCAL_BUILD=1` says so; anywhere else it says SKIPPED, never passed. The check is `tools/local_build.py`, one canonical text kept by `tools/sync_repo_tools.py` (`--check` compares the copies); `tools/host_tests.py` (and the relay golden check that uses it) asks it first. The build scripts of the repository are the explicit way to build and are not locked. Tests of the lock added.

- **Slot scan contract, to find two boards set to the same slot:** `docs/SLOT_SCAN.md` defines a probe (offset `0x1E`) and a reply (offset `0x1F`) inside the window of every slot, in which a board delays its answer by a number of steps taken from its own unique identifier, so two boards with the same board identifier answer one after the other instead of colliding. `tools/slot_scan.py` is the host reference (frames, delay and the verdict `ABSENT`, `OK`, `DUPLICATE`, `MISPLACED` or `NOISY` of each slot; only `OK` lets an update write), `docs/bus_contract.json` carries its numbers, `tests/slot_scan_vectors.json` holds the frames and nine scenarios and `tests/test_slot_scan.py` checks them against the identifier table. The firmware side is in the entries above.

- **The SPI link and the mailbox between the cores are sealed:** the padding of the 128-byte SPI frame now carries a marker, a sequence number and a CRC-16/CCITT-FALSE (`spi_frame_crc.h`, the same file in the bootloader of the CM4 and in the driver of the CM5; `tools/spi_frame.py` is the reference, `tests/spi_frame_vectors.json` the vectors). A damaged frame is dropped, a retry is not run twice, a jump of the sequence is counted, and a frame of the older kind is still accepted unless the build sets `HYDRA_SPI_REQUIRE_CRC=1`. The mailbox frame between the two cores is sealed the same way (`HYDRA_IPC_REQUIRE_CRC`). See `docs/SPI_FRAME_INTEGRITY.md`.
- **Failed set-up calls are no longer ignored:** the status of the FDCAN, filter, start, SPI, clock and watchdog calls of the bootloaders and of the applications is checked. A bootloader whose link failed still starts a valid application but takes no update, and without an application stops refreshing the watchdog so the reset tries again; a bootloader or application whose watchdog cannot be armed stops instead of running unguarded; an application whose bus set-up fails switches the motion outputs off and stops.
- **The FDCAN hardware filters accept only what the node needs:** the controller board takes the 32 identifiers of its own slot (mask `0x7E0`) and the emergency stop `0x001`, the kinematic brain the windows of the eight slots and `0x001`; everything else is rejected by the peripheral instead of filling the receive FIFO.
- **Motion outputs fail safe:** the six driver enable outputs of the controller board are forced off by register writes when the application stops (failed set-up, stack overflow, hard fault), the blink task refreshes the watchdog only while the relay task (the STACK A task of the brain) shows signs of life, and the board needs a pull-up on every enable line (bench test 26). `tools/rdp_plan.py` decodes the read-out protection byte, prints the programmer command for level 0 or 1 (level 2 is refused) and names a chip by a hash of its unique ID; see `docs/PRODUCTION_PROTECTION.md`. Bench tests 26 to 29 are not executed.
- **The text references of the hardware documentation are Markdown files:** the pinout, CAN bus, build and board parts-list references under `docs/` and `hardware/PCB/` now use the `.md` extension (the original plain text sits in a `text` block, so ASCII tables keep their layout), and every link to them in the README files, the source comments, the build script and the firmware manifest follows.
- **The manual firmware build takes the trust table as an input and measures the size of the bootloaders:** the `flavour` input of `.github/workflows/firmware-build.yml` (`dev`, the default, or `release`) replaces the fixed development-key build; the build now fails unless each of the three bootloaders holds exactly the trust table of its flavour (`tools/image_trust.py check`), and the release table stays closed until the image-signing key exists. A new step runs `tools/bootloader_size.py`: it reads the ELF of each bootloader with `arm-none-eabi-size`, takes the flash and RAM regions from its linker script, writes a table in the job summary and fails when a bootloader leaves less free space than the margins of `tools/bootloader_size.json`.
- **The three bootloaders verify header format 2 against a table of public keys and hold no secret:** `bootloader_trust.[ch]` decodes the trust table the linker places in `.trust_keys` at offset `0x400` (generated from `docs/image_trust.json` by `tools/image_trust.py headers`; the bench build compiles the development table with `-DHYDRA_TRUST_DEV`, an official build the release table, which is closed until the image-signing key exists). `UpdateVerify_Decide2` picks the key by the key id of the header, checks the Ed25519 signature over `hydra-umc-image-sig/2 || SHA-256(header || image)`, the header against what arrived, and a signed rollback counter that nothing overrides (the downgrade token only lifts the version rule). Header format 1 is no longer accepted by the bootloaders. `HYDRA_FW_HMAC_KEY_HEX` and `HYDRA_FW_UPDATE_PUBKEY_HEX` are gone: the integrity check of the installed application is a plain SHA-256 kept in the metadata.
- **The metadata is an append-only log:** records of generation, state and CRC-32 are appended without an erase; the valid record with the highest generation is the state, so a power cut during a write leaves the previous state in force. The G474 alternates between its two pages (the newest state is never without a valid copy); the H745 cores use their sector and erase it only when it is full.
- **Host tests for all of it:** the G474 whole-update test runs the shared cases of `tests/image_format2_vectors.h` (made by the host reference) through the decision, the key policy (next, revoked, closed table, bad CRC), the counter and the metadata; the log is cut at every double-word of a record and of the change of page; `tests/test_bootloader_trust.c` runs once per bootloader copy in both flavours. `docs/BENCH_TESTS.md` lists bench tests 22 to 25 (not executed).
- **Header format 2, the trust table and their public source, with a Python reference and vectors:** `tools/image_header2.py` fixes the 64-byte header with the rollback counter and the key id inside the signed bytes, the signature domain of format 2 (different from format 1's) and the order in which a bootloader with a trust table decides an image; `tools/image_trust.py` builds the `TKEY` table (56 or 100 bytes, current, next and revoked keys), validates `docs/image_trust.json`, checks that a compiled bootloader holds exactly that table at its fixed offset, and prints only the public entry of a seed file for the procedure that provisions the image-signing key. `tests/image_format2_vectors.json` (written by `tools/gen_image_format2_vectors.py`, `--check` in the tests) holds 23 decision cases and the table cases with throwaway test keys. Format 1 and the bootloaders are unchanged; the `release` table stays empty until the key is provisioned.
- **Emergency stop contract on identifier 0x001:** `docs/ESTOP_CAN.md` defines the highest-priority frame of the stack bus (trip, release and alive, 8 bytes, epoch and sequence against replay, fail-safe for anything malformed), the manual and acknowledged re-arm, the safe state of each kind of node, the timing and its relation to a hardware stop and to the jog dead-man. `docs/bus_contract.json` carries its numbers, `tools/estop_can.py` is the host reference, `tests/estop_vectors.json` holds the frames and scenarios and `tests/test_estop_contract.py` checks them. The firmware side is in the entries above, and the open decisions are listed in the page.

- **Bench test script:** `docs/BENCH_TESTS.md` lists the checks of the CAN tunnel to a URTC head, the FDCAN clock and the over-the-air update that need a real bus, all marked not executed. It also lists the checks of the dual-core mailbox, the controller board clocks and standby pins, the SPI link, the update of several boards, the emergency stop and the loss of power, all marked not executed.

## [0.2.6] - The build signs the application images, the C host tests run the same way on every platform and the Python fakes are checked against the firmware

- **The build signs the application images:** `python tools/build.py` signs each application image (`HYDRA_<board>_APP_v<version>.bin`) with Ed25519 after a successful build and writes the signature next to it as a `.sig` file (`tools/sign_release.py`). The private seed is only read from a file outside the repository (`HYDRA_FW_SIGNING_SEED_FILE`) and is never printed; without it the images stay unsigned and the build says so, and `HYDRA_UMC_RELEASE=1` turns that into an error. `python tools/sign_release.py verify` checks the signatures against the public key the bootloaders are built with (`HYDRA_FW_UPDATE_PUBKEY_HEX`).

- **The C host tests run the same way on every platform:** `python tools/build.py --host-tests` (or `python tools/host_tests.py`) compiles the real bootloader and relay sources with the host C compiler (`HOST_CC`, `cc`, `gcc` or `clang`) and runs them, on Linux, macOS and Windows. Nothing is compiled for a board. A test checks that the table of tests lists every `tests/test_*.c`.

- **The Python fakes are checked against the firmware:** `tests/relay_golden_dump.c` compiles the relay logic of the controller board (`relay_ids.h`) for the PC, `python tools/gen_relay_golden.py` writes what it answers into `tests/relay_golden_vectors.json`, and `tests/test_relay_golden.py` requires the CM5 fragmenting code, the fake controller board of the bridge tests and the CAN identifier table to produce the same bytes. With a host compiler the test also fails when the file is older than the firmware.

## [0.2.5] - The repository bumps like every other one, one entry point builds the firmware on any platform and the retired Qt6 kiosk shell is gone

- **The repository now bumps like every other one:** `python tools/hydra_project_tool.py bump` used to stop at the repository's own `bump_version.py`, which wanted a header and a macro family on the command line. Run without arguments, that script now bumps the application version of the header that the manifest names as its native version (the one the repository version follows), and the manifest follows with `bump_manifest_version.py --sync`, so a batch of releases can include this repository. With a header and a family it works exactly as before, and the other five component versions are still raised on their own with `bump_firmware_versions.py`.

- **The Qt6 kiosk shell `src/cm5_host/hmi_qt6/` is retired**, together with its placeholder unit `os/systemd/hydra-hmi.service`: it was a second full-screen browser next to the HDMI kiosk of HYDRA-UMC-OS, it never ran on a screen and it pointed at a port that the dashboard is not served on. The HDMI screen is shown by the kiosk of HYDRA-UMC-OS and the touch screen by HYDRA-UMC-DSI. The architecture picture, the file trees, the editor recommendations and the README sources no longer mention it.

- **One entry point to build the firmware on any platform:** `python tools/build.py [--clean] [--ci] [--check] [all|g474|h745]` runs `build_firmware.sh` or `build_firmware.bat` for the platform. `--check` builds nothing: it fails when the two scripts pin different versions of the third-party sources and reports which tools are present. `tests/test_build_entry.py` runs the same comparison in the CI. The two scripts are still two files.

## [0.2.4] - Valid standby pins proposed for the G474 controller board, a bus contract a program can read, the explained deployment target and the universal README

- **The pinout of the STM32G474 robot controller board marks the CAN standby pins as invalid and proposes valid ones:** the LQFP-64 package has no PG7 or PG8, and PB8 (FDCAN1 RX) is also the BOOT0 pin. `docs/PINOUT_STM32G474_ROBOT_CONTROLLER.md` now proposes PC5 and PC8 for the two standby lines and the option bytes that take BOOT0 from the option byte (`nSWBOOT0 = 0`, `nBOOT0 = 1`). It is a proposal pending schematic review: no board has been made and the firmware still names the old pins.

- **A bus contract a program can read:** `docs/bus_contract.json` records, for STACK A, the downlink to a tool head and the SPI link to the CM5, the frame format, the bit timing the firmware sets, the clock it assumes (and the bit rate that follows from both), the identifier window of a slot and the largest frame. `tests/test_bus_contract.py` compares it with the FDCAN initialisation of the five firmware sources, with the slot base of both bootloaders, with `tools/can_ids.py` and with the SPI driver, so a change in one of them that is not made in the other fails. The data length of each message is not recorded yet. No bit rate has been measured on a board.
- **README in the universal layout:** the README and its seven translations follow the layout that the four families share (22 sections in a fixed order), with an architecture picture and a repository-structure picture drawn from `docs/architecture.json` and `docs/structure.json`; the CI validator checks both pictures and the file tree.

- **The layout of the README is checked in the CI:** the validator runs `tools/universal_readme_check.py` and lists, as a warning, what differs from the universal layout of the README.

### Documentation

- The manifest explains its deployment target (`deployment_target_note`): the firmware runs on the microcontrollers of the robot arm boards, and `user-pc` is the nearest value the manifest allows for the tools and the CM5-side bridge.
- `docs/architecture.md` no longer marks the relay tunnel between the controller board and the Tool Head as implemented: the code exists, but the frame identifiers have not been agreed with URTC and nothing has run on a real bus.

## [0.2.3] - The CM5 side holds no signing key and the CAN error counters of the boards reach the telemetry collector

### Firmware and protocol

- **The bootloaders authenticate an update with an Ed25519 signature instead of an HMAC.** The board holds only a public key (`UPDATE_PUBLIC_KEY`), so reading its flash no longer gives anybody the power to make an update. The signature (eight `HMAC_CHUNK` frames of eight bytes) is over a domain text plus the SHA-256 digest of the 64-byte header and the image, which the bootloader hashes out of its flash; the downgrade token is a second signature (eight frames) over the nonce of the session, the two versions and the digest of the image. The same file changes in the three bootloaders (`update_verify.h`, `image_header.h`, `update_signature.[ch]`) and the vendored TweetNaCl (`tweetnacl.[ch]`, public domain, noted in `THIRD_PARTY_NOTICES.md`) does the curve arithmetic; the watchdog is refreshed while it works. The metadata HMAC stays as the local integrity check of the installed application.
- **Keys.** `tools/image_header.py` now makes the keys (`keygen`, `pubkey`) and signs (`sign`); `tools/ed25519_ref.py` is a pure Python RFC 8032 implementation checked against its vectors and OpenSSL. A build takes the public key from `HYDRA_FW_UPDATE_PUBKEY_HEX`; without it the bootloaders carry a development public key whose seed is published, and `HYDRA_UMC_RELEASE=1` refuses that. The production keys are made by the owner; the tests use made-up keys.
- **The CM5 side holds no signing key.** `SpiOtaFlasher` sends the signature it is given (`signature=`, or the `signature` parameter of `POST /flash`) or has a signer the deployment provides make it; a HYDRA-UMC board without one is refused before anything is sent, and a downgrade needs a signer for the token of the session. The URTC bootloaders behind the relay keep the HMAC.
- **The CAN error counters of the boards reach the telemetry collector.** The application of the controller board now answers the error counter query like its bootloader (`relay_ids.h`, host tested); the SPI bridge reads the counters (`query_error_counters`, `GET /counters`) and `python -m spi_bridge serve --collector-url ... --counter-target 2:3` sends one `can_error_counters` sample per board and round to `POST /ingest/ws` of the collector, with the token from a file or the environment, sharing the lock of the SPI link with the HTTP service. A board that does not answer produces no sample, only a count. Tested against fake transports and a fake collector; nothing has run on a board.
- **The three bootloaders take the signed image header and apply the anti-rollback rule.** `image_header.h` and `update_verify.h` (one file each, identical in `src/mcu_stm32g474/boot`, `src/mcu_stm32h745/CM4/boot` and `CM7/boot`) decide an update in a fixed order: everything arrived, the CRC, the HMAC tag over header and image (checked before anything in the header is believed), the header against what the board knows (hardware id, link address, size, CRC, vector words, RAM) and strictly newer by major.minor.patch. The version is the header's, not the one in END_UPDATE; the metadata keeps the patch (`version_patch`, a page from before reads as 0). The four constant bytes that switched the anti-rollback off are ignored; a downgrade needs a token bound to the nonce of the session (asked for with `OFS_DOWNGRADE_NONCE_REQUEST`, answered on `OFS_DOWNGRADE_NONCE_RESPONSE`), the two versions and the image. New frame types 0x19-0x1C (in `tools/can_ids.py`, one emitter each) and failure reason 0x06. `bootloader_crypto` gains the HMAC over a buffer followed by a flash region. The header layout now holds the 32-bit hardware id. Tested with the host C compiler: the C code against bytes made by `tools/image_header.py` (`tests/image_header_vectors.h`, generated by `tools/gen_image_header_vectors.py`), the real HMAC against the reference, and the whole update of the G474 from START_UPDATE to the new application; the two H745 protocol sources compile against a stand-in HAL and `tests/test_update_glue_parity.py` keeps the glue of the three identical. The CM5 client (`src/cm5_host/spi_bridge`) builds the header, the tag and the token for the HYDRA-UMC boards (`version_patch` and `downgrade_from=x.y.z` on `/flash`) and keeps the old format and frame for the URTC boards behind the relay. Not compiled for the boards and not run on one; the HMAC key is still symmetric (the Ed25519 replacement needs a C library and keys); STUDIO, SUITE and the server relay must send the patch of the version.
- Added `tools/image_header.py`, the host reference of the signed firmware image header and the anti-rollback rule: version (major, minor, patch), hardware id, link address, size, vector words and CRC are inside a 64-byte header, the tag covers header and image together (so an old signed image cannot declare a higher version), the three numbers of the version are compared, and a downgrade needs a token derived from the key, a session nonce and the exact pair of versions. 17 tests. The bootloader in C still uses the old scheme: this fixes the format and the decisions before it is ported.
- **The relay protocol uses one emitter per CAN identifier.** The controller board answers the axis status request (+0x10) on +0x16 and the relay receive request (+0x13) on +0x18 (`relay_ids.h`, host tested); the kinematic brain waits for those; the CM5 asks a gateway for the next page acknowledge or status of a board with the new SPI frame type POLL_STATUS (+0x17), which the CM4 gateway never puts on the CAN bus, and the gateway refuses to transmit the identifiers a board transmits. The SPI bridge client and the relay tunnel follow (a poll through the tunnel only drains the receive queue). `tools/can_ids.py` is the table and tests compare it with the three bootloader headers, the firmware sources and the bridge. The three sources compile for the boards (syntax only) and the host logic runs; nothing has run on a board, and STUDIO and SUITE still have to follow the new frame types where they talk to the relay.
- `LICENSE` is now the full GPL-3.0 text, so the repository's licence is recognised; the licence split (firmware and tools GPL-3.0-or-later, hardware CERN-OHL-S v2, documentation CC BY-SA 4.0) is in `LICENSE.md`.

### Tests, builds and checks

- Host tests: the Ed25519 check against the RFC 8032 vectors and the signatures the reference made, the watchdog being fed, a signature of somebody else's key, a scalar that is not reduced, the whole G474 update with signatures, and the digest of the image against the reference. Nothing was compiled for a board or run on one.
- `build_firmware.sh` and `build_firmware.bat` no longer raise any version: they report the version each of the six firmware components is at (read with the new `show_component_version.py`) and build with it. Raising versions is the explicit `python bump_firmware_versions.py` (all six components, or the ones named; `--list` shows them), which uses `bump_version.py` and then moves the project manifest with the G474 application. A test fails if a build script runs a bump again.
- The validator, the shared documentation policy and the project tool are the shared copies again; the validator now also checks that no build script bumps a version and that no script activates a virtual environment.
- Added `tools/hydra_project_tool.py`: the same `validate`, `test`, `version`, `status` and `bump` commands in every repository. The checks run with `HYDRA_UMC_CI=1` and fail if the version changes; only `bump` changes it.

### Documentation

- The six per-component changelogs (the bootloader and the application of each microcontroller), frozen at 0.0.7 and only pointing at each other, are removed: this file is the only changelog of the repository.
- `README_por.md` carries the safety and security status note like the other languages, and the shared documentation and README parity checks now apply to this repository too.

### Other changes

- The banner follows the animated style of the ecosystem banners, with the name, the kind and the technologies of the project.

## [0.2.2]

- Maturity is scaffolding: the applications are still skeletons and nothing here has been verified on real hardware yet.
- The READMEs say, in the eight languages, what is planned and what has only been tested against fakes, emulators or fixtures, and carry a short safety and security status note.

## [0.2.1] - HYDRA-UMC-DOCS in the related projects

- The Related Projects section of the README, in the eight languages, lists HYDRA-UMC-DOCS: the documentation and tooling hub of the ecosystem (architecture, interfaces, security baseline, capability matrix and a project catalogue generated from every manifest).
- No firmware component was rebuilt for this version: the version of the representative application component is raised so that the manifest, the native version source and the changelog stay in step.

## [0.2.0]

- Build version synchronized with `hydra-umc.project.json` and the repository-native version source.
- The licence is written as GPL-3.0-or-later in every header and in the package metadata.

## [0.1.9] - The firmware now agrees with the chips: watchdog window, CAN lengths, flash banks, CM4 link address

None of this has run on a bench yet (no board exists): every firmware change below compiles clean with the ARM build and, where the logic does not need the chip, is covered by a host test.

- **Watchdog window.** All six watchdog set-ups left the window at 0, and a refresh with the counter above the window resets the chip, so the first refresh of every bootloader and application would have reset the board in a loop. They now set `IWDG_WINDOW_DISABLE`. A source-guard test fails the build if a watchdog set-up ever leaves it out.
- **CAN data length.** The FDCAN HAL takes and returns the plain 0-8 code and shifts it itself; the bootloaders shifted it a second time on transmit (a data length that points outside the HAL's table) and every receive path shifted it back (always reading 0). All paths now use one pair of helpers, `DlcToFdcanDataLength()` / `FdcanDataLengthToDlc()`, with a host test that sends through a model of the message-RAM word and reads it back.
- **Flash banks of the STM32G474.** The part is two banks of 128 pages and the erase command takes the bank plus a 7-bit page number, but the code passed a flat page number to bank 1, so the first update would have erased the bootloader's own pages instead of the backup slot. The address is now split into (bank, page) (`boot/flash_geometry.h`, host-tested), and the bootloader refuses to erase unless the `DBANK` option bit is set (the factory default).
- **CM4 application link address.** The CM4 application was linked into the metadata sector (`0x08120000`) while its bootloader copies and starts it at `0x08140000`; the linker script now matches `MAIN_APP_ADDR`, and the stack-pointer check of the CM4 bootloader looks at the RAM the application is really linked into (`0x30000000`, not the `0x10000000` alias). `generate_manifest.py` now refuses to write a manifest when an image's `.hex` does not start at its slot, or its first vector words point outside its RAM or slot.
- **Relay frames.** A tool-head frame of 6-8 bytes arrives as two RELAY_SEND fragments; the Robot Controller Board forwarded each as its own short frame, while the tool head's bootloader only accepts whole 8-byte (or 4-byte) frames. The fragments are now put back together into one frame (`relay_send_reassembly.h`, host-tested), and a target id above `0x7FF` is dropped. On the CM5 side the tunnel translates the bootloader commands to the tool head's CAN ids through an explicit table (the readback shares `0x7FE`/`0x7FF`, and the commands the tool head does not have are refused instead of becoming an id outside the CAN range).
- **A refused update request no longer holds the board.** Any node on the bus that sent a start-update frame in the first 600 ms made the bootloader stay in update mode even when the request was refused (wrong size or hardware id). Only an accepted request leaves the normal start-up now.
- **Reset cause per core.** The two cores of the STM32H745 each read and clear their own copy of the reset status, so one no longer clears the cause the other is about to read.
- **Relay on the Kinematic Brain.** Timeouts use elapsed time (still right when the 32-bit tick wraps), a full transmit FIFO no longer makes the relay poll spin, and a malformed fragment run is dropped instead of waited out.
- **Signing key in builds.** The conversion of `HYDRA_FW_HMAC_KEY_HEX` into bytes lost the digits (`0x,0x,...`), so a build with a real key could not compile; and the two Kinematic Brain bootloaders never received the key, so they always carried the public development key. Both are fixed, and `build_firmware.bat` now accepts the key and refuses a release build without it, like the shell script.
- **Windows build.** `build_firmware.bat` never compiled `RobotControllerRelay.c`, so the Robot Controller Board application did not link; it does now.
- **Version helper.** `bump_version.py` asked the project manifest whether to hand over to the shared utility and would have broken every component bump once the project version gained its fourth part; it now looks only at the header it is given (and bumps a fourth `_BUILD` macro if the header has one). Covered by a test.
- **Build safety.** The build compiles into a staging folder and moves the binaries into `firmware/` only when everything passed, so a failed compile no longer leaves `firmware/` empty. The `.hex`, `.bin` and `.elf` files are marked as non-text so their sizes in the manifest stay valid on every platform.
- **SPI bridge.** One SPI operation at a time (`409` while a flash runs), `tier` 0-2 and `slot` 0-7 checked, `version_major`/`version_minor` required for `POST /flash`, a corrupt reply is an error instead of an exception that killed the stream, and the data-ready wait consumes the edge it saw (it used to return at once from the second wait on).
- **Smaller fixes.** The CM5 IPC driver clocks an idle frame (`0xFF`) instead of zeros, which the bootloader reads as a command; MediaMTX paths use `source: publisher` and the API listens on loopback only; the dashboard unit that started a script STUDIO does not have is gone, and the kiosk unit waits for the real server service; wrong comments (watchdog tick, SRAM sizes, the never-run host test) corrected; `.vscode/tasks.json` and `extensions.json` are explicitly kept in git.
- **CI.** The firmware inventory check, the SPI bridge tests and the new source-guard tests now run in CI, and the ARM toolchain install names `libnewlib-arm-none-eabi`.
- **Documentation.** Every README states that there is no E-STOP, software limit, interlock, power-loss dump or motion command in the firmware yet; `SECURITY.md` lists the supported version; the CAN and compile references describe the real flash layout, the relay and the output folder.

## [0.1.8] - The relay empties the FDCAN FIFOs

- **No more frames lost between two passes.** The Robot Controller Board's relay took one message out of each FDCAN receive FIFO per pass and then slept, while the hardware FIFO of the STM32G4 holds only three frames: a burst (the fragments of an HMAC transfer, the tool head's telemetry) lost frames in silence. A pass now takes up to eight messages from each FIFO, and the uplink handler is its own function.

## [0.1.7] - A healthy board is no longer parked at its fourth start, and the bootloader key comes from the build

- **Boot counting.** Every start used to add one to the count of unconfirmed starts and nothing ever confirmed an application healthy, so the bootloader refused to start the application at the fourth power-up of a perfectly good board. The count now only climbs across watchdog resets (a hang or a spinning fault handler); power-ups, button resets and software restarts are clean starts that bring it back to 0 and write nothing to flash (which also ends the erase of the metadata page at every start). A crash-looping application is still tried three times and then parked, and the next power-up gives it another try. The three bootloaders share the logic, covered by host tests (ten power-ups of a healthy board, and a crash loop that is parked after three tries).
- **Confirm from the CM5.** `confirm_healthy()` in the SPI bridge sends the confirmation frame (`OFS_CONFIRM_HEALTHY`) that lets a person clear a parked board, with its constant and a test.
- **The signing key is a build input.** `build_firmware.sh` takes `HYDRA_FW_HMAC_KEY_HEX` and hands it to the bootloaders; without it the build carries the development key that is published in the repository, warns loudly, and `HYDRA_UMC_RELEASE=1` refuses it.

## [0.1.6] - Pre-hardware readiness: SPI-OTA DoS cap, real HYDRA_DATA_READY handshake, boot-attempt counter and FDCAN2 capture-ring host tests

- **Real DoS cap on the SPI-OTA flash upload route, real HYDRA_DATA_READY
  handshake in the CM5-side IPC driver, and 3 doc/code drift fixes.**
  `src/cm5_host/spi_bridge/spi_bridge/http_service.py`'s `POST /flash` used to call
  `self.rfile.read(content_length)` straight off the client-supplied `Content-Length`
  header with no upper bound - a malicious or malformed client could claim a
  multi-gigabyte body and block/OOM the handler thread. It now rejects anything
  above `MAX_FIRMWARE_BODY_BYTES` (4 MiB, well above the largest real target flash,
  the STM32H745's own 2 MB) with a clean `413` before ever reading the body; new
  regression test proves it. `src/cm5_host/ipc_driver/src/ipc_driver.c` (the CM5 <->
  STM32H745 SPI userspace driver) used to poll the SPI bus unconditionally, with a
  TODO left where the real `HYDRA_DATA_READY` GPIO handshake should be -
  `hydra_ipc_open()`/`hydra_ipc_read_frame()` now really request that line via
  libgpiod v2's edge-event API and block on a real rising edge (or `timeout_ms`)
  before transferring, instead of reading whatever happens to be on the bus at
  poll time; still unverified against real hardware (no STM32H745 firmware exists
  yet to assert the line), but the handshake itself is real code now. Also fixed:
  `docs/HYDRA-UMC_README.md`/`docs/HYDRA-UMC_TECHNICAL.md` claimed FDCAN1 auto
  bus-off recovery was already "managed by the Cortex-M4" - it is not implemented
  yet (the root `README.md`/its 6 translations already said so correctly; only
  these two docs had drifted); and the SPI1 "up to 50 MHz" claim in `README.md`
  and its 6 translations now states plainly that the real CM5-side driver defaults
  to a conservative 10 MHz until the STM32H745's own SPI1 slave-side config is
  verified against real hardware, matching `ipc_driver.c`'s own comment.

- **All 3 real bootloaders (Robot Controller Board / G474,
  Kinematic Brain CM4, Kinematic Brain CM7 - BOOTLOADER_VERSION bumped to
  0.1.5/0.1.4/0.1.4) gained a real boot-attempt counter and refuse-to-jump
  safeguard.** The real update mechanism (backup-slot verify -> CRC32 +
  HMAC -> copy to main slot) already refused to ever install an unverified
  image, and the anti-rollback check already refused a silent downgrade -
  neither caught a DIFFERENT real failure: a cryptographically valid,
  correctly-flashed application that is simply BROKEN at runtime.
  `ApplicationIsValid()` only ever checked the metadata's own state/magic,
  never whether the code it points at actually runs correctly once jumped
  to. New `boot_decision.h` (a real per-target copy, byte-identical across
  all 3, like `dlc_to_fdcan_data_length.h`): `FirmwareMetadata_t` gains a
  `boot_attempts` field, incremented and persisted to flash right before
  every real jump; a new CAN/SPI command (`OFS_CONFIRM_HEALTHY`, offset
  0x15) resets it back to 0 - sent by an external master (never the
  application itself, which would need every application firmware in this
  ecosystem to link in this bootloader's own flash-write code, real,
  separate, NOT-yet-designed future work this change stays out of scope
  of). After `BOOT_MAX_ATTEMPTS` (3) consecutive un-confirmed boots, the
  bootloader refuses to jump again and stays in listening mode instead
  (`STATUS_ROLLBACK_SUSPECT`) - fails CLOSED and stays recoverable over
  the bus, never a silent automatic restore of a prior image (this
  codebase does not keep a real "golden slot" anywhere yet - that is
  separate, larger future work, not conflated with this fix). This is a
  genuinely new design for this ecosystem - no existing project here
  already has a boot-counter/confirm pattern to port from (checked against
  both this project's own prior bootloader and sibling repo URTC's - URTC
  does not have this yet either, real, separate future work if full
  ecosystem parity is wanted later).

  Verification, stated precisely: the real `arm-none-eabi-gcc` toolchain
  this project already depends on confirmed clean compilation (`-Wall
  -Wextra`, 0 warnings) of every modified source against all three real
  target include paths, AND a full real link into a working bootloader
  `.elf` for all three. The new host-only pure-logic test
  (`tests/test_boot_decision.c`, wired into `build_firmware.sh`'s own
  host-tests step, run 3x - one per target, same pattern as
  `test_dlc_to_fdcan_data_length.c`) was syntax-checked clean against all
  three targets but NOT actually executed this session - no native host C
  compiler was available in this environment; it must run for real
  (compile AND execute) the next time `build_firmware.sh` itself runs on
  a real Linux host. Real hardware validation (an actual crash-loop-then-
  recover cycle on a physical board) remains separate, larger, entirely
  unaddressed future work (a physical-bench scope on its own).

- **Host logic tests for the Robot Controller Board's FDCAN2 capture ring**
  (new `tests/`). The ring buffer that `RobotControllerRelay.c` fills from
  FDCAN2 and drains for `RELAY_RECV` was inline module state - no way to
  test its contract without a G474 board. It is now a HAL-free header,
  `src/mcu_stm32g474/relay_rx_queue.h` (`static inline`, only `<stdint.h>`
  / `<string.h>` - the generated code is identical, only the single-
  instance state moved into a struct), which a host test compiles and
  exercises with plain `gcc`: FIFO order, wrap-around past the fixed
  depth, the **drop-the-oldest-when-full** policy (never the newest -
  the property the `RELAY_RECV` drain path depends on), and DLC-over-8
  clamping. Wired into `build_firmware.sh` as step "1b" (skipped cleanly
  when no host C compiler is present). Verified compiling and passing
  under `gcc 14.2 -std=c11 -Wall -Wextra`, and the full
  `arm-none-eabi-gcc` firmware build re-run to confirm the extraction did
  not change the G474 target build.
- **Real bug fixed in `src/cm5_host/spi_bridge/spi_bridge/http_service.py`**
  (`hydra-umc-spi-bridge` 0.1.0 -> 0.1.1): `POST /flash`'s `do_POST()`
  called `int(self.headers.get("Content-Length", "0"))` with no
  try/except at all - a malformed (non-numeric) `Content-Length` header
  raised an uncaught `ValueError` in the request-handler thread, closing
  the connection with no HTTP response at all instead of the clean `400`
  every other malformed param on this route already gets (`tier`/`slot`/
  `hardware_id`/`version_major`/`version_minor` were already guarded).
  Confirmed live with a raw socket (urllib always computes a real
  Content-Length itself, so this needed a hand-built request to
  reproduce) before and after the fix. New regression test in
  `tests/test_http_service.py`; python -m pytest: 32 passed (was 31).
- **`hydra-umc.project.json`** - `maturity` was `"experimental"`, a value
  HYDRA-UMC-UPDATER's own manifest validator has never accepted
  (`VALID_MATURITY = {"scaffolding", "functional", "established",
  "production"}`, `project_manifest.py`). A manifest that fails
  validation is silently excluded from remote discovery
  (`github_client.py`'s own `_fetch_discovered_manifest`) - this repo,
  the ecosystem's own flagship firmware project, had been invisible on
  the public dashboard for real, not by design. Changed to `"functional"`,
  the closest honest match to this repo's real status: real, compiling,
  linking firmware across all 6 targets, genuinely not yet verified
  against real silicon. Verified against the real validator
  (`hydra_umc_updater.project_manifest.parse_manifest`) - parses clean.

- **`os/README.md`** - stopped presenting "Raspberry Pi OS vs. Yocto" as
  still an open decision. The separate repo
  [HYDRA-UMC-OS](https://github.com/JuanenRac/HYDRA-UMC-OS) already
  builds on Raspberry Pi OS ARM64 for real (a real device agent, real
  non-secret config, a real hardened systemd unit, a real tested
  `provisioning/first_boot.sh` and install scripts, a real idempotent
  preflight check and rollback mechanism) - this folder's own
  `systemd/`/`first_boot/` placeholders predate that repo and were never
  updated to reflect it, leaving two contradictory "OS strategy"
  documents in the ecosystem. Documentation-only fix - no firmware
  binary changed, so no version/artifact bump; `os/README.md` itself
  documents the real reconciliation work (retiring these placeholders in
  favor of HYDRA-UMC-OS) still ahead.
- **Added `src/cm5_host/hmi_qt6/src/kiosk_view.{h,cpp}`** (new) - real
  kiosk lockdown for the `QWebEngineView` shell, written against Qt6's
  real, documented API: no right-click context menu
  (`setContextMenuPolicy(Qt::NoContextMenu)`), no accidental close
  (`closeEvent()` overridden to `ignore()`, covering both the window
  manager's own Alt+F4 delivery and a stray close() call without needing
  to trap raw keystrokes), Escape/F11 swallowed as defense-in-depth,
  cursor hidden after 5s idle (a touch panel has no persistent pointer
  need) - unlocked only by a technician combo
  (Ctrl+Alt+Shift+Q). Also added `loadWithRetry()`, a real
  retry-until-loaded flow so a cold-booting CM5 whose dashboard Node
  server isn't up yet keeps retrying instead of showing a browser
  connection-error page once. **`main.cpp`** now builds a real (drawn in
  code, no image asset yet) `QSplashScreen`, kept on screen through
  however many retries that flow needs, closed only once the dashboard
  genuinely finishes loading. **Now genuinely verified by actual
  compilation**: real Qt 6.7.3 (`win64_msvc2019_64`, via `aqtinstall`)
  with the real `WebEngineWidgets` module, built with
  `cmake -B build -G "Visual Studio 16 2019" -A x64` +
  `cmake --build build --config Release` against this repo's own
  `CMakeLists.txt` - a real, unmodified `hydra_hmi.exe` came out the
  other end, no source changes needed. **Now also verified on the real
  CM5/Linux target**: the same unmodified CMake project, built on a real
  Debian 13 (trixie) aarch64 CM5 against the real `qt6-base-dev`/
  `qt6-webengine-dev` (6.8.2) Debian packages, produced a real ARM64 ELF
  `hydra_hmi` binary - see that folder's own README.md for what's still
  left (a real on-screen run against the CM5's own HDMI output/GPU
  driver stack).

## [0.1.5] - A.11: DlcToFdcanDataLength extracted to a HAL-free header + host tests

- **`DlcToFdcanDataLength()`** was a private `static` function duplicated
  identically inside both `RobotControllerRelay.c` (Robot Controller
  Board, G474) and `KinematicBrainCan.c` (Kinematic Brain CM4, H745) -
  the same real DLC-to-byte-length table, un-testable without a board
  and silently able to drift between the two copies. Pulled out into a
  HAL-free `dlc_to_fdcan_data_length.h` (`static inline`, only
  `<stdint.h>` - same real pattern `relay_rx_queue.h` already
  established, see the entry above), one real per-target copy in each
  MCU's own directory (this project builds each target from its own
  isolated include path - a genuinely shared file across G474/H745
  isn't possible here). The exact same host test source
  (`tests/test_dlc_to_fdcan_data_length.c`) is compiled and run TWICE
  by `build_firmware.sh`'s own step "1b" - once per target's own copy -
  so a real behavioral drift between the two would be a real test
  failure, not just unnoticed. Confirmed with an injected regression
  (a wrong table value) failing correctly, reverted before committing.
  Full `arm-none-eabi-gcc` firmware build re-run for all 6 components -
  bootloaders and applications for both boards - to confirm the
  extraction changed no target binary's own real behavior.

## [0.1.4] - Real Robot Controller Board (G474) relay tunnel application

- **Added `src/mcu_stm32g474/RobotControllerRelay.{h,c}`** (new) - the
  real application-side logic `docs/architecture.md` section 6's own
  status table explicitly flagged as missing: "the Robot Controller
  Board's own APPLICATION-side relay logic (not its bootloader) that
  actually speaks to the URTC head is still not written." Real FDCAN2
  init (downlink to this robot's own URTC Tool Head,
  `docs/PINOUT_STM32G474_ROBOT_CONTROLLER.md` section 1b), a real
  AXIS_STATUS (+0x10) responder reading this board's own 6 endstop + 3
  fault-sense GPIOs (`docs/CANBUS_STM32G474.md`'s own documented
  txData[0]/[1] layout), and a real RELAY_SEND/RELAY_RECV (+0x12/+0x13)
  tunnel forwarding opaque CAN traffic between FDCAN1 (uplink, STACK A)
  and FDCAN2 - the exact real 2-fragment-max scheme (2-byte target/source
  CAN ID BE + 1-byte total DLC + up to 5 data bytes) already implemented,
  to the same spec, on both `src/mcu_stm32h745/CM4/KinematicBrainCan.c`
  (Tier 0, the bus master this tunnel talks to) and
  `src/cm5_host/spi_bridge/spi_bridge/relay_tunnel.py` (CM5). A bounded,
  drop-oldest 8-entry FIFO buffers FDCAN2 traffic between RELAY_RECV
  polls - no dynamic allocation, matching this project's own embedded
  convention throughout. `ReadSlotBaseId()`/FDCAN1 init are real,
  intentional duplicates of the bootloader's own already-proven
  functions of the same name (two separate linked binaries, same
  precedent `KinematicBrainCan.c` already established for Tier 0).
  `OFS_ENTER_BOOTLOADER` (+0x00) is deliberately still a no-op - a real,
  separately-tracked gap (needs a boot-selection mechanism that doesn't
  exist anywhere in this project yet), not silently glossed over.
  `STM32G474RE_main.c` now runs a real second FreeRTOS task
  (`vRelayTask`) polling this module, alongside the original
  `vBlinkTask` smoke test.
- Verified with the real toolchain (`arm-none-eabi-gcc` 10.3.1): isolated
  compile of both new/changed files with zero warnings, then the full,
  real `build_firmware.sh all` incremental build across all 6 MCU
  targets - compiles and links clean end-to-end (41 passed, 0 warnings,
  0 failed), `firmware/firmware_manifest.json` regenerated and verified
  against the real artifacts (`tools/verify_firmware_inventory.py`).
  `docs/architecture.md` section 6's own status table updated to
  IMPLEMENTED for this row.

## [0.1.2] - Real SPI-OTA bridge (CM5 side) + real FDCAN1 STACK A application on the Kinematic Brain

- Added committed `firmware/firmware_manifest.json`, generated from the six
  versioned MCU artifacts with their real byte counts and CRC32 values.
- Added `tools/verify_firmware_inventory.py`; the non-mutating build check now
  fails if the manifest is incomplete or any binary/HEX/ELF artifact no
  longer matches the recorded version, size or checksum.
- Fixed `tools/build_test.py`: it previously invoked the mutating firmware
  build, whose required cleanup could remove committed artifacts. Build-test
  now performs only the read-only inventory verification; `build_firmware.*`
  remains the explicit incremental build and packaging command.
- **Added `src/cm5_host/spi_bridge/`** (new, Python) - a real CM5-side
  SPI-OTA implementation, replacing `src/cm5_host/ipc_driver/`'s own
  unfinished C skeleton (kept, not deleted - see that folder's own README
  for why). Reuses the sibling `URTC-FLASHER` tool's own real, already
  byte-verified CRC32/HMAC-SHA256 bootloader state machine, ported from
  CAN framing to this project's real 128-byte `SpiOtaFrame_t` SPI framing
  (byte-for-byte matching `mcu_stm32h745/CM4/boot/bootloader_common.h`'s
  own C struct): real frame pack/unpack, a real `spidev`+`gpiod`
  transport (lazily imported), the full real ENTER_BOOTLOADER →
  START_UPDATE → HMAC_CHUNK ×4 → DATA → END_UPDATE → STATUS flash cycle,
  and a small local HTTP service `HYDRA-UMC-SERVER` now relays to (`POST
  /api/hardware/canota/flash`, `GET /api/hardware/canota/version`). 22
  deterministic tests against an in-memory fake transport - no real
  SPI/GPIO/STM32H745 hardware required to prove the state machine is
  correct.
- **Added `src/cm5_host/spi_bridge/spi_bridge/relay_tunnel.py`** - reaches
  Tier 2 (the URTC Tool Head) through its own Robot Controller Board's
  real RELAY_SEND/RELAY_RECV tunnel (architecture.md section 5), with a
  real, explicit 5-byte fragmentation scheme for tunneling a real CAN
  frame's up-to-8-byte data through the SPI-OTA `SpiOtaFrame_t`'s own
  8-byte payload field. `RelayedTransport` implements the exact same
  `SpiOtaTransport` interface the direct Tier 0/1 transport does, so
  `bootloader_client.py`'s already-tested state machine reaches a real
  Tier 2 target completely unchanged - no new protocol logic needed on
  that side. Found and fixed a real design bug while wiring this up:
  waiting on a RELAY_RECV response for fire-and-forget frame types
  (ENTER_BOOTLOADER/START_UPDATE/HMAC_CHUNK/DATA, which
  `bootloader_client.py` never reads a response for) would have added a
  real, needless multi-second timeout to hundreds of frames across a
  single flash cycle - fixed by only waiting on the 3 frame types the
  real protocol actually responds to (QUERY_VERSION/PAGE_ACK/STATUS).
  `http_service.py`'s routes gained a `relay=1` query parameter. Tier 3
  (Advanced Expansion Board) needs one further real tunnel hop (URTC's
  own I2C bridge, CAN IDs 0x210-0x221) - not implemented yet, same real
  pattern would apply. 9 new regression tests, including a faithful fake
  Tier-1-relaying-to-Tier-2 stand-in - 31/31 tests passing.
- **`HYDRA-UMC-STUDIO`/`HYDRA-UMC-SERVER`** (separate repos) now reach
  Tier 0/1 for real through this service, and Tier 2 through the new
  relay tunnel above, when `settings.canOta.transport === 'hardware'` -
  see those repos' own CHANGELOG entries.
- **`src/mcu_stm32h745/CM4/STM32H745ZI_CM4_main.c`** - real `SystemClock_Config()`:
  HSI64 → PLL1 (M=8/N=120/P=2, `RCC_PLL1VCIRANGE_2`, wide VCO) → SYSCLK
  480MHz → HCLK 240MHz, matching this project's own BOM (no external HSE
  crystal on this board; Cortex-M4@240MHz target). Deliberately only on
  this core, not also CM7 (whose own placeholder is unchanged) - PLL1 is
  a single chip-wide resource, and this chip's real dual-core boot is two
  fully independent resets with no CM7-releases-CM4 handshake in this
  design, so both cores configuring it independently would be a real
  register-level race; resolving that for CM7 too needs a real HSEM-gated
  handshake, out of scope for what this session's work needed (this core
  reaching the G474 boards for real, not CM7's own motion engine).
- **Added `src/mcu_stm32h745/CM4/KinematicBrainCan.{c,h}`** (new) - real
  FDCAN1 "STACK A" master application logic, reusing the bootloader's own
  already-proven `MX_FDCAN1_Init()`: real slot-addressed `AXIS_STATUS`
  query (+0x10, new offset - architecture.md section 4's own reserved
  range) and the real RELAY_SEND/RELAY_RECV tunnel (+0x12/+0x13) to Tier
  2, using the exact same fragmentation scheme as `relay_tunnel.py` above
  (kept consistent by construction - both written this session against
  the same spec). Found while implementing this: the real *receiving* end
  of the tunnel (unpacking a RELAY_SEND fragment and forwarding it out a
  Robot Controller Board's own second CAN controller to the URTC head)
  belongs in `src/mcu_stm32g474/`'s own firmware, not here - this core
  only ever builds/sends RELAY_SEND and polls/reassembles RELAY_RECV as
  the tunnel's client side. Not implemented in this session - flagged as
  the next real step, not left ambiguous.
- Verified with the real, non-mutating toolchain this repo already uses
  (`arm-none-eabi-gcc` 10.3.1) and then the real, full `build_firmware.sh`
  incremental build - compiles and links clean end-to-end (41 passed, 0
  warnings, 0 failed), the strongest verification possible without real
  hardware. `vBlinkTask`/GPIO-toggle behavior is unchanged; the new
  `vStackATask` round-robins a real `AXIS_STATUS` query across all 8
  slots - a real timeout on an unpopulated/unresponsive slot is expected
  behavior today, not an error, since no Robot Controller Board exists on
  a real bus to answer yet.

## [0.1.1]

- Build version synchronized with `hydra-umc.project.json` and the repository-native version source.

## [0.1.0]

- Build version synchronized with `hydra-umc.project.json` and the repository-native version source.

## [0.0.9]

- Build version synchronized with `hydra-umc.project.json` and the repository-native version source.

## [0.0.8]

- Build version synchronized with `hydra-umc.project.json` and the repository-native version source.

## [0.0.1]
### Added
- Multi-language READMEs (English, Spanish, French, Italian, German).
- Industrial branding badges to all documentation.
- Selection bar for quick language switching.
- Standardized `CONTRIBUTING.md`, `SECURITY.md`, `SUPPORT.md`, and `CODE_OF_CONDUCT.md`.

### Fixed
- Fixed documentation inconsistencies between firmware tiers.
- Improved pinout description for the Kinematic Brain STM32H745.

## [0.0.0]
### Added
- Initial system architecture with 4-tier design.
- STM32H745ZIT6 (Kinematic Brain) bring-up firmware.
- STM32G474RET6 (Robot Controller Board) bring-up firmware.
- FDCAN1 "STACK A" protocol definition.
- High-speed SPI IPC driver skeleton for CM5.
