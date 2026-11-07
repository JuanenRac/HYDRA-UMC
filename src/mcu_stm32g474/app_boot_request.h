/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32g474/app_boot_request.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The application of a controller board answering ENTER_BOOTLOADER (HAL-free, host-testable).
 *
 * The host client sends ENTER_BOOTLOADER (offset 0x00, payload B0 07 1D 5A) to a board before it flashes it. The bootloader answers it; the
 * application used to ignore it, so a board that was running its application could not be updated over the bus without a power cycle. This
 * file holds the decision and the hand-over:
 *
 *   - the application accepts the command only with the exact magic, only with a payload of 4 bytes, and only while the drivers are off
 *     (the emergency stop is latched): a board that moves never resets because of a frame;
 *   - it then writes a request word into a register that survives a software reset (the backup register 0 of the TAMP block) and resets;
 *   - the bootloader reads the word at the start, clears it, and stays in its update window instead of jumping to the application.
 *
 * The request word is a magic with its complement, so a register that holds random bits after a power-up is not a request.
 * Everything is behind HYDRA_APP_ENTER_BOOTLOADER (default 0): with 0 the application ignores the command as before and the bootloader does not
 * read the register. Evidence of the tests: simulator (tests/test_app_support.c); the reset itself is bench row 14.
 * ========================================================================== */
#ifndef HYDRA_UMC_APP_BOOT_REQUEST_H
#define HYDRA_UMC_APP_BOOT_REQUEST_H

#include <stdint.h>

#ifndef HYDRA_APP_ENTER_BOOTLOADER
#define HYDRA_APP_ENTER_BOOTLOADER 0
#endif

#define APP_BOOT_MAGIC_0 0xB0u
#define APP_BOOT_MAGIC_1 0x07u
#define APP_BOOT_MAGIC_2 0x1Du
#define APP_BOOT_MAGIC_3 0x5Au
#define APP_BOOT_REQUEST_WORD      0xB007F00Du
#define APP_BOOT_REQUEST_COMPLEMENT (~APP_BOOT_REQUEST_WORD)

typedef enum {
    APP_BOOT_IGNORED = 0,        /* not this command, or the switch is off */
    APP_BOOT_REFUSED_BAD_MAGIC,  /* wrong payload or length */
    APP_BOOT_REFUSED_MOVING,     /* the drivers are enabled: never reset a board that may be moving */
    APP_BOOT_ACCEPTED            /* the request word is written and the caller resets */
} AppBootResult;

/* `request` is the register that survives a software reset (on the board: &TAMP->BKP0R; on the host: a variable). Two words are used, at
 * request[0] and request[1]. `drivers_off` is 1 when the emergency stop is latched. */
static inline AppBootResult AppBoot_OnEnterBootloader(volatile uint32_t *request, const uint8_t *payload, uint8_t dlc, uint8_t drivers_off)
{
#if !HYDRA_APP_ENTER_BOOTLOADER
    (void)request; (void)payload; (void)dlc; (void)drivers_off;
    return APP_BOOT_IGNORED;
#else
    if (dlc != 4u || payload[0] != APP_BOOT_MAGIC_0 || payload[1] != APP_BOOT_MAGIC_1 || payload[2] != APP_BOOT_MAGIC_2 || payload[3] != APP_BOOT_MAGIC_3)
        return APP_BOOT_REFUSED_BAD_MAGIC;
    if (!drivers_off) return APP_BOOT_REFUSED_MOVING;
    request[0] = APP_BOOT_REQUEST_WORD;
    request[1] = APP_BOOT_REQUEST_COMPLEMENT;
    return APP_BOOT_ACCEPTED;
#endif
}

/* The bootloader at the start: 1 when the application asked for an update, and the request is cleared so it applies once. */
static inline uint8_t AppBoot_TakeRequest(volatile uint32_t *request)
{
#if !HYDRA_APP_ENTER_BOOTLOADER
    (void)request;
    return 0u;
#else
    const uint8_t asked = (uint8_t)(request[0] == APP_BOOT_REQUEST_WORD && request[1] == (uint32_t)APP_BOOT_REQUEST_COMPLEMENT);
    request[0] = 0u;
    request[1] = 0u;
    return asked;
#endif
}

#endif /* HYDRA_UMC_APP_BOOT_REQUEST_H */
