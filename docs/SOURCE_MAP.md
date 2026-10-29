# Source map

Every file of the repository with a line on what it does: the annotated tree that the README carried before it was reduced to the first level (the first level, in eight languages, is in the section Repository Structure of the README).

```text
├── .vscode/                    # Recommended extensions + build tasks - see "Development Environment" below
├── docs/
│   ├── datasheets/             # Datasheets of parts used across every board in this repo
│   ├── architecture.md         # The 4-tier system architecture (start here)
│   ├── COMPILE_STM32G474.TXT   # Robot Controller Board firmware build reference
│   ├── COMPILE_STM32H745.TXT   # Kinematic Brain firmware build reference (dual-core)
│   ├── PINOUT_STM32H745_KINEMATIC_BRAIN.TXT    # Kinematic Brain full pin allocation
│   ├── PINOUT_STM32G474_ROBOT_CONTROLLER.TXT   # Robot Controller Board full pin allocation
│   ├── PINOUT_CM5_CARRIER.TXT                  # CM5 host subsystem signal routing
│   ├── PINOUT_STACKA_CONNECTOR.TXT             # Shared 40-pin STACK A stacking connector
│   ├── CANBUS_STM32H745.TXT                    # Kinematic Brain wire-level protocol (SPI1/mailbox/FDCAN1-master)
│   ├── CANBUS_STM32G474.TXT                    # Robot Controller Board wire-level protocol (FDCAN1-slave/FDCAN2)
│   └── HYDRA-UMC_*.md/txt/TXT  # Older docs - Markdown where authored as Markdown; see each file's own banner
├── hardware/
│   ├── PCB/
│   │   ├── kinematic_brain_stm32h745/          # Main motherboard - no schematic yet, see its own README
│   │   └── robot_controller_board_stm32g474/   # Per-robot board - no schematic yet, see its own README
│   └── gerbers/                # Manufacturing output files (empty until a board is laid out)
├── src/                         # Same layout convention as the sibling URTC repo: src/ is SOURCE
│   ├── cm5_host/                # Linux userspace apps that run ON TOP of os/'s own image
│   │   ├── hmi_qt6/             # Qt6 kiosk shell wrapping HYDRA-UMC-STUDIO's own dashboard
│   │   ├── ai_inference/        # Hailo-8 TAPPAS / YOLOv8 pipeline
│   │   ├── video_streamer/      # Multi-camera RTSP/WebRTC server (MediaMTX)
│   │   ├── ipc_driver/          # CM5 <-> STM32H745 SPI link (userspace) - unfinished C skeleton, kept for reference
│   │   └── spi_bridge/          # Real CM5<->STM32H745 SPI-OTA bridge (Python) - replaces ipc_driver/,
│   │                              adapts URTC-FLASHER's own proven CRC32/HMAC bootloader state machine
│   ├── mcu_stm32h745/           # Kinematic Brain firmware (Tier 0) - dual-core
│   │   ├── CM7/                 # Motion engine, hardware timers (+ its own boot/)
│   │   ├── CM4/                 # FDCAN drivers, sensor filtering (+ its own boot/)
│   │   └── Common/              # CM7<->CM4 shared-memory IPC mailbox (ipc_mailbox.h) - implemented, used by both cores' bootloaders
│   └── mcu_stm32g474/           # Robot Controller Board firmware (Tier 1) - single-core, + its own boot/
├── tests/                   # Host tests: C logic of the bootloaders and the relay (stubs/ stands in for the HAL, image_header_vectors.h holds the reference vectors) and Python tests of the tools
├── os/                          # CM5 OS image - base OS choice, systemd units, first-boot provisioning
├── images/                      # README banner + icon + splashscreen (SVG)
├── build_firmware.sh            # Builds every MCU firmware target above from a clean checkout (Linux/Mac)
├── build_firmware.bat           # Same build, Windows (see "Building the Firmware" below)
├── build-test.sh / build-test.bat # Non-versioning build/compile check
├── generate_manifest.py         # Regenerates firmware/firmware_manifest.json (versions/CRC32) after a full build
├── bump_version.py              # Version bump of one component
├── bump_firmware_versions.py    # Raises the six component versions, explicitly
├── show_component_version.py    # Prints the version a component already has
├── bump_manifest_version.py     # Syncs hydra-umc.project.json's version to the native one (--sync)
├── tools/
│   ├── verify_firmware_inventory.py # Read-only verification of the committed six-component inventory
│   ├── build_test.py                # Non-versioning build/compile check
│   ├── ci_validate.py               # Manifest/CHANGELOG/docs validation used by CI
│   ├── image_header.py              # Host reference of the signed firmware image header, its Ed25519 signature and the anti-rollback rule
│   ├── gen_image_header_vectors.py  # Writes tests/image_header_vectors.h: the signed images and downgrade token the reference makes, as C arrays
│   ├── ed25519_ref.py               # Ed25519 (RFC 8032) in pure Python: the host side of the signature the bootloaders verify
│   ├── can_ids.py                   # Reference table of the CAN identifiers of the relay protocol, one emitter per identifier
│   ├── _doc_policy.py               # Documentation rules shared by the family (a copy of HYDRA-UMC-SDK's, kept in sync)
│   ├── _readme_parity.py            # Check that the eight READMEs share one structure (a copy of HYDRA-UMC-SDK's, kept in sync)
│   └── hydra_project_tool.py        # The same commands in every repository: validate, test, ci, version, status, bump
├── firmware/                    # Committed build output (.bin/.hex/.elf + manifest) - NOT gitignored, same convention as URTC's own output folder, see "Building the Firmware" below
├── README.md                    # This file
└── README_spa.md / README_ita.md / README_fra.md / README_deu.md / README_zho.md / README_jpn.md / README_por.md    # <- translations
```

See `docs/architecture.md` for what each tier actually does and how they
connect; every folder above with its own `README.md` has more detail than
this top-level summary.
