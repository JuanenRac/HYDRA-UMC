# Changelog - Kinematic Brain CM4 Bootloader (`src/mcu_stm32h745/CM4/boot/`)

Versioned independently from the application firmware - flashing a new
bootloader doesn't imply a new application version and vice versa. The
application's own changelog lives at
[`../CHANGELOG.md`](../CHANGELOG.md).

Per this repo's ecosystem-wide versioning policy, THIS component is fully
incremental and auto-bumped: `build_firmware.sh`/`.bat` bump
`BOOTLOADER_VERSION_PATCH` by exactly 1 in `bootloader_common.h`,
immediately before compiling this bootloader, via `bump_version.py`
("odometer" carry rule - PATCH past 9 resets to 0 and MINOR increments by
1, e.g. `1.1.9` -> `1.2.0`, never `1.1.10`; MINOR past 9 carries into MAJOR
the same way). Unlike sibling repo URTC, where bootloader versions are
bumped by hand before a build counts as final, here every real build that
produces a new binary bumps automatically - no manual step, never able to
drift out of sync with what was actually compiled.

| Version | Notes |
|---|---|
| **1.0.2** | Verification build - second of two consecutive builds run to confirm `bump_version.py` correctly increments this bootloader's own version build over build. No source changes to this bootloader itself. |
| **1.0.1** | Verification build - first of two consecutive builds run to confirm the automatic version-bump mechanism increments `BOOTLOADER_VERSION_PATCH` correctly and that the bumped value is what actually gets baked into the compiled binary and its output filename. No source changes to this bootloader itself. |
| **1.0.0** | Initial versioned release. Real CAN-OTA gateway bootloader (SPI1 to the CM5, FDCAN1 to STACK A, mailbox to CM7 - see this component's own `bootloader_protocol.c`), implementing CRC32 + HMAC-SHA256 verify-into-backup-before-copy-to-main - compiling clean end to end, not yet verified against real hardware. |
