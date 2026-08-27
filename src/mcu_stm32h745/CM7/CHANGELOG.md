# Changelog - Kinematic Brain CM7 Application (`src/mcu_stm32h745/CM7/`)

Versioned independently from the bootloader - the bootloader's own
changelog lives at [`boot/CHANGELOG.md`](boot/CHANGELOG.md).

Per this repo's ecosystem-wide versioning policy, THIS component is fully
incremental and auto-bumped, same as the bootloader: `build_firmware.sh`/
`.bat` bump `FIRMWARE_VERSION_PATCH` by exactly 1 in `boot/
bootloader_common.h`, immediately before compiling this application, via
`bump_version.py` (same "odometer" carry rule as the bootloader - PATCH
past 9 resets to 0 and MINOR increments by 1, e.g. `0.1.9` -> `0.2.0`,
never `0.1.10`). Unlike sibling repo URTC, where only bootloaders are
incremental (and there, bumped by hand), here all 6 HYDRA-UMC components -
3 bootloaders AND 3 applications - auto-bump this way. Previously this
application only carried `FIRMWARE_VERSION_MAJOR`/`_MINOR` (no PATCH field
at all); `FIRMWARE_VERSION_PATCH` was added to `boot/bootloader_common.h`
as part of making this component incremental.

| Version | Notes |
|---|---|
| **0.0.7** | Verification build - confirms the automatic version-bump mechanism still increments this application's own version correctly after an unrelated maintenance pass. No source changes to this application itself. |
| **0.0.6** | Same fix as the G474 application's own 0.0.6 entry (hallazgo #110, full-ecosystem audit): this application inherits the watchdog already armed by its own bootloader (survives `HAL_DeInit()`) but never touched it - added this application's own `IWDG_HandleTypeDef` (matching the bootloader's Instance/Prescaler/Reload) plus a periodic refresh in its own task loop, avoiding a perpetual reset loop on real hardware. (0.0.4 was a verification build, no source changes; 0.0.5 was an orphaned intermediate build superseded by this one.) |
| **0.0.4** | Verification build - same build-mechanism verification pattern as 0.0.1-0.0.3. No source changes to this application itself. |
| **0.0.3** | Verification build - confirms `build_firmware.bat` (the Windows mirror of `build_firmware.sh`) calls the exact same `bump_version.py` step and increments this application's own version identically to the Linux/Mac script. No source changes to this application itself. |
| **0.0.2** | Verification build - second of two consecutive builds run to confirm `bump_version.py` correctly increments this application's own version build over build. No source changes to this application itself. |
| **0.0.1** | Verification build - first of two consecutive builds run to confirm the automatic version-bump mechanism increments `FIRMWARE_VERSION_PATCH` correctly (the field itself is new as of this same change - see this file's own header note) and that the bumped value is what actually ends up in the compiled binary's output filename. No source changes to this application itself. |
| **0.0.0** | Initial versioned release (previously `MAJOR.MINOR` only, no PATCH field). Still a FreeRTOS GPIO-toggle smoke test, not yet the real motion/vision application logic - see `../README.md` for current status. |
