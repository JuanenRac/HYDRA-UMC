// =============================================================================
// HYDRA-UMC Bootloader - boot_rng.h: the hardware random generator mixed into the nonce of a session
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The G474 and the H745 have the same random generator (control, status and data registers with the same bits). BootRng_Mix works on
// three register addresses, so the host test (tests/test_boot_rng.c) drives it with made-up registers. The words of the generator are
// XORed over what the caller already had: a generator that never answers, or that reports a seed or a clock error, leaves the nonce no
// weaker than before, and nothing waits longer than the spin bound. Switch HYDRA_BOOT_NONCE_HW_RNG, off as it ships: with it off no
// bootloader includes anything of this file in its image. Nothing here has been run on a board.
// =============================================================================
#ifndef BOOT_RNG_H
#define BOOT_RNG_H

#include <stdint.h>

#define BOOTRNG_CR_RNGEN (1UL << 2)
#define BOOTRNG_SR_DRDY (1UL << 0)
#define BOOTRNG_SR_CECS (1UL << 1)
#define BOOTRNG_SR_SECS (1UL << 2)
#define BOOTRNG_SR_CEIS (1UL << 5)
#define BOOTRNG_SR_SEIS (1UL << 6)
#define BOOTRNG_SR_ERRORS (BOOTRNG_SR_CECS | BOOTRNG_SR_SECS | BOOTRNG_SR_CEIS | BOOTRNG_SR_SEIS)

// How many reads of the status register one word may take before the generator is given up.
#ifndef BOOTRNG_SPINS
#define BOOTRNG_SPINS 100000UL
#endif

typedef struct {
    volatile uint32_t *cr;
    volatile uint32_t *sr;
    volatile const uint32_t *dr;
} BootRng_t;

// Mixes up to `count` words of the generator into `words` (XOR) and returns how many were mixed. It stops at the first error, time-out
// or zero word (the sign of a failed generator), and always switches the generator off again.
static inline uint32_t BootRng_Mix(const BootRng_t *rng, uint32_t *words, uint32_t count, uint32_t spins)
{
    uint32_t done = 0;
    *rng->cr |= BOOTRNG_CR_RNGEN;
    while (done < count) {
        uint32_t left = spins;
        uint32_t status = *rng->sr;
        while ((status & (BOOTRNG_SR_DRDY | BOOTRNG_SR_ERRORS)) == 0u && left != 0u) {
            left--;
            status = *rng->sr;
        }
        if ((status & BOOTRNG_SR_ERRORS) != 0u || (status & BOOTRNG_SR_DRDY) == 0u) break;
        const uint32_t value = *rng->dr;
        if (value == 0u) break;
        words[done] ^= value;
        done++;
    }
    *rng->cr &= ~BOOTRNG_CR_RNGEN;
    return done;
}

#endif // BOOT_RNG_H
