# Changelog: HYDRA-UMC Core 🦾

All notable changes to the hardware and core firmware will be documented in this file.

## [Unreleased]

- **Nothing compiles on the PC of the developer by accident:** a build that needs a C compiler, Cargo, Go, Gradle or an npm build runs only on a CI runner (`CI` or `GITHUB_ACTIONS` set) or when `ALLOW_LOCAL_BUILD=1` says so; anywhere else it says SKIPPED, never passed. The check is `tools/local_build.py`, one canonical text kept by `tools/sync_repo_tools.py` (`--check` compares the copies); `tools/host_tests.py` (and the relay golden check that uses it) asks it first. The build scripts of the repository are the explicit way to build and are not locked. Tests of the lock added.

- **Slot scan contract, to find two boards set to the same slot:** `docs/SLOT_SCAN.md` defines a probe (offset `0x1E`) and a reply (offset `0x1F`) inside the window of every slot, in which a board delays its answer by a number of steps taken from its own unique identifier, so two boards with the same board identifier answer one after the other instead of colliding. `tools/slot_scan.py` is the host reference (frames, delay and the verdict `ABSENT`, `OK`, `DUPLICATE`, `MISPLACED` or `NOISY` of each slot; only `OK` lets an update write), `docs/bus_contract.json` carries its numbers, `tests/slot_scan_vectors.json` holds the frames and nine scenarios and `tests/test_slot_scan.py` checks them against the identifier table. Contract only: no firmware answers the probe yet and nothing has run on a board.

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
- **Emergency stop contract on identifier 0x001:** `docs/ESTOP_CAN.md` defines the highest-priority frame of the stack bus (trip, release and alive, 8 bytes, epoch and sequence against replay, fail-safe for anything malformed), the manual and acknowledged re-arm, the safe state of each kind of node, the timing and its relation to a hardware stop and to the jog dead-man. `docs/bus_contract.json` carries its numbers, `tools/estop_can.py` is the host reference, `tests/estop_vectors.json` holds the frames and scenarios and `tests/test_estop_contract.py` checks them. Contract only: no firmware sends or honours it yet, and the open decisions are listed in the page.

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
