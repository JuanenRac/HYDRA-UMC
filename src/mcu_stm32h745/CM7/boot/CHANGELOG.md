# Changelog - Kinematic Brain CM7 Bootloader (`src/mcu_stm32h745/CM7/boot/`)

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
| **1.0.6** | Same fix as the G474 bootloader's own 1.0.6 entry (hallazgo #110, full-ecosystem audit): `HAL_IWDG_Init()` was being called after `HAL_Init()`/`SystemClock_Config()` in `main()`, leaving startup unwatched until that point - moved to the first line of `main()`. (1.0.4 was a verification build, no source changes; 1.0.5 was an orphaned intermediate build superseded by this one.) |
| **1.0.4** | Verification build - same build-mechanism verification pattern as 1.0.1-1.0.3. No source changes to this bootloader itself. |
| **1.0.3** | Verification build - confirms `build_firmware.bat` (the Windows mirror of `build_firmware.sh`) calls the exact same `bump_version.py` step and increments this bootloader's own version identically to the Linux/Mac script. No source changes to this bootloader itself. |
| **1.0.2** | Verification build - second of two consecutive builds run to confirm `bump_version.py` correctly increments this bootloader's own version build over build. No source changes to this bootloader itself. |
| **1.0.1** | Verification build - first of two consecutive builds run to confirm the automatic version-bump mechanism increments `BOOTLOADER_VERSION_PATCH` correctly and that the bumped value is what actually gets baked into the compiled binary and its output filename. No source changes to this bootloader itself. |
| **1.0.0** | Initial versioned release. Real CAN-OTA bootloader, mailbox-relayed (this core has no bus of its own - see this component's own `bootloader_protocol.c`), implementing CRC32 + HMAC-SHA256 verify-into-backup-before-copy-to-main - compiling clean end to end, not yet verified against real hardware. |
