/* =============================================================================
 * HYDRA-UMC - tests/test_boot_rng.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The mix of the hardware random generator into the nonce (firmware_base/boot/boot_rng.h) with made-up registers: it XORs the words,
 * stops at an error, a silent generator or a zero word, and always switches the generator off. Evidence: simulator.
 * ========================================================================== */
#include <assert.h>
#include <stdio.h>
#include "boot_rng.h"

#define SPINS 16u

static volatile uint32_t cr, sr, dr;
static const BootRng_t fake = { &cr, &sr, &dr };

static void reset(uint32_t status, uint32_t value)
{
    cr = 0x20u; /* another bit of the control register, which has to stay */
    sr = status;
    dr = value;
}

int main(void)
{
    uint32_t words[4] = { 1u, 2u, 3u, 4u };

    reset(BOOTRNG_SR_DRDY, 0xA5A5A5A5u);
    assert(BootRng_Mix(&fake, words, 4u, SPINS) == 4u);
    assert(words[0] == (1u ^ 0xA5A5A5A5u) && words[3] == (4u ^ 0xA5A5A5A5u));
    assert(cr == 0x20u);

    /* a generator that never answers leaves the words as they were */
    uint32_t kept[4] = { 1u, 2u, 3u, 4u };
    reset(0u, 0xFFFFFFFFu);
    assert(BootRng_Mix(&fake, kept, 4u, SPINS) == 0u);
    assert(kept[0] == 1u && kept[3] == 4u && cr == 0x20u);

    /* every error bit wins over data ready */
    const uint32_t errors[4] = { BOOTRNG_SR_CECS, BOOTRNG_SR_SECS, BOOTRNG_SR_CEIS, BOOTRNG_SR_SEIS };
    for (unsigned i = 0u; i < 4u; i++) {
        reset(BOOTRNG_SR_DRDY | errors[i], 7u);
        assert(BootRng_Mix(&fake, kept, 4u, SPINS) == 0u);
        assert(kept[0] == 1u && cr == 0x20u);
    }

    /* a zero word is the sign of a failed generator */
    reset(BOOTRNG_SR_DRDY, 0u);
    assert(BootRng_Mix(&fake, kept, 4u, SPINS) == 0u);
    assert(kept[1] == 2u && cr == 0x20u);

    puts("boot_rng: ok");
    return 0;
}
