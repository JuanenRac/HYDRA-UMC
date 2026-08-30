# Changelog: HYDRA-UMC Core 🦾

All notable changes to the hardware and core firmware will be documented in this file.

## Unreleased

- Added committed `firmware/firmware_manifest.json`, generated from the six
  versioned MCU artifacts with their real byte counts and CRC32 values.
- Added `tools/verify_firmware_inventory.py`; the non-mutating build check now
  fails if the manifest is incomplete or any binary/HEX/ELF artifact no
  longer matches the recorded version, size or checksum.
- Fixed `tools/build_test.py`: it previously invoked the mutating firmware
  build, whose required cleanup could remove committed artifacts. Build-test
  now performs only the read-only inventory verification; `build_firmware.*`
  remains the explicit incremental build and packaging command.

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
