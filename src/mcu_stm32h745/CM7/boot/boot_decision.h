// =============================================================================
// HYDRA-UMC Bootloader - pure, host-testable boot-attempt decision logic
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// the real update mechanism (backup-slot verify -> CRC32 +
// HMAC -> copy to main slot, see bootloader_flash.c's own Flash_CopyRegion)
// already refuses to ever copy an unverified image into the main slot, and
// HandleEndUpdate()'s own anti-rollback check already refuses to silently
// downgrade. Neither of those catches a DIFFERENT real failure mode: a
// cryptographically valid, correctly-flashed application that is simply
// BROKEN at runtime (hangs, crash-loops, resets itself) - ApplicationIsValid()
// only ever checks the metadata's own state/magic, never whether the code it
// points at actually runs correctly once jumped to.
//
// This is a NEW design for this ecosystem - no existing project here already
// has a boot-counter/confirm pattern to port from (verified against both
// this project's own prior bootloader and URTC's "already proven" one, see
// this file's own git history/CHANGELOG for that investigation) - so it is
// deliberately the simplest, most conservative version of a real, common
// embedded pattern (see e.g. MCUboot's own "swap with confirm" mode for the
// general shape), NOT a from-scratch invention:
//
//   - `boot_attempts` (FirmwareMetadata_t's own new field) counts how many
//     times in a row this bootloader has jumped to the CURRENT application
//     without it ever being confirmed healthy.
//   - Every real jump increments and persists it to flash BEFORE jumping -
//     so a crash-and-reset immediately after jump is captured even though
//     the bootloader itself never regains control afterward otherwise.
//   - `OFS_CONFIRM_HEALTHY` (a new CAN command, bootloader_protocol.c's own
//     HandleConfirmHealthy()) resets it back to 0 - sent by an external CAN
//     master (HYDRA-UMC-OS's own agent, or a human) once it has observed the
//     application behaving normally for a while, never by the application
//     itself: that would need every application firmware in this ecosystem
//     to link in this bootloader's own flash-write code, a much larger,
//     separate, NOT-yet-designed cross-cutting change this fix deliberately
//     stays out of scope of.
//   - Once `boot_attempts` reaches BOOT_MAX_ATTEMPTS, this bootloader
//     refuses to jump again and stays in listening mode instead
//     (STATUS_ROLLBACK_SUSPECT) - fails CLOSED (stays recoverable over CAN),
//     never a silent, automatic restore of a PREVIOUS image: this codebase
//     does not keep one anywhere today (BACKUP_APP_ADDR only ever holds the
//     NEXT candidate image mid-update, never a prior known-good one) -
//     building and maintaining a real "golden slot" is real, separate,
//     larger future work, not conflated with this fix.
//
// VERIFICATION STATUS, stated precisely rather than reused from this
// project's own usual "host-tested" phrase, because this one real time it
// means something narrower: the real arm-none-eabi-gcc cross-compiler this
// project already depends on confirmed clean compilation (-Wall -Wextra, 0
// warnings) of every real modified bootloader source file (bootloader_main.c/
// bootloader_common.h/bootloader_protocol.c/.h) against all THREE real
// target include paths, AND a full real link into a working bootloader
// .elf for all three (g474/CM4/CM7) - not merely "it looks right". What
// this did NOT get, this session: tests/test_boot_decision.c's own pure
// logic was syntax-checked (compiles clean against all three targets) but
// never actually EXECUTED - no native host C compiler was available in
// this environment to run it the way build_firmware.sh's own host-tests
// step (a genuinely different, native compiler from the ARM cross-compiler
// above) normally would; it must run for real the next time
// build_firmware.sh itself runs on a real Linux host, before this is
// treated as equivalently verified to the rest of this project's own
// host-tested logic. Real hardware validation (an actual crash-loop-then-
// recover cycle on a physical board) is separate, larger, still entirely
// unaddressed future work (this project's own "physical bench" scope).
// =============================================================================
#ifndef BOOT_DECISION_H
#define BOOT_DECISION_H

#include <stdint.h>

// How many consecutive un-confirmed boots of the SAME application this
// bootloader tolerates before refusing to jump to it again. Deliberately
// small (a genuinely healthy application confirms quickly; a crash-looping
// one reveals itself within 1-2 resets) and a plain #define (not
// configurable at runtime) - the same "an operator decides this ahead of
// time, never something a caller/attacker can widen" discipline this whole
// ecosystem already applies to allow-lists elsewhere.
#define BOOT_MAX_ATTEMPTS 3

// Pure decision, no hardware/flash access of its own - the real Metadata_Read()/
// Metadata_EraseAndWrite() calls stay in bootloader_main.c, exactly where
// every other real flash access in this bootloader already lives. Returns
// 1 (jump) or 0 (refuse, stay in listening mode).
static inline uint8_t BootDecision_ShouldJump(uint8_t app_valid, uint32_t boot_attempts) {
    if (!app_valid) {
        return 0;
    }
    return (boot_attempts < BOOT_MAX_ATTEMPTS) ? 1 : 0;
}

#endif // BOOT_DECISION_H
