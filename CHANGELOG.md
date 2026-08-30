# Changelog: HYDRA-UMC Core 🦾

All notable changes to the hardware and core firmware will be documented in this file.

## [Unreleased] - Pre-hardware readiness: os/ reconciled, real hmi_qt6 kiosk lockdown

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
  genuinely finishes loading. Explicitly **NOT verified by actual
  compilation** - no Qt6 (specifically no `WebEngineWidgets`, the
  Chromium-based module this needs) is installed on this development
  machine, and a real Qt6 + WebEngine install is multi-gigabyte, judged
  too large to fetch just to verify this one app this session; see that
  folder's own README.md for the exact verification still needed before
  trusting this builds.
  documents the real reconciliation work (retiring these placeholders in
  favor of HYDRA-UMC-OS) still ahead.

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
