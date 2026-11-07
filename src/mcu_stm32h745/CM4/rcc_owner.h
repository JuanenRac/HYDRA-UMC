/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h745/CM4/rcc_owner.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Who sets up the clock tree of the STM32H745 (HAL-free, host-testable). The RCC is shared by the two cores: if both applications configure PLL1,
 * the second one changes the clock under the first while it runs (the CM7 sees its system clock move, and so do the timings of the mailbox between
 * the cores). The rule here is the one ST's own examples use, with a hardware semaphore of the HSEM block:
 *
 *   1. the core that takes the semaphore `HYDRA_HSEM_RCC` first sets the tree up;
 *   2. a core that finds the system clock already on the PLL does nothing (the other core did it, or this core restarted alone: its semaphore is
 *      still held but the clock is running, and configuring it again would move the clock under the other core);
 *   3. a core that did not get the semaphore and finds the clock not ready waits a bounded time for the other core, then carries on with the clock
 *      it has (the reset default) and records that it did.
 *
 * The decision only needs two facts, both read by the caller: "my take of the semaphore succeeded" and "the system clock source is the PLL".
 * The firmware uses it behind HYDRA_RCC_HSEM_HANDSHAKE (default 0: the CM4 application sets the tree up on its own, as before).
 * ========================================================================== */
#ifndef HYDRA_UMC_RCC_OWNER_H
#define HYDRA_UMC_RCC_OWNER_H

#include <stdint.h>

#ifndef HYDRA_RCC_HSEM_HANDSHAKE
#define HYDRA_RCC_HSEM_HANDSHAKE 0
#endif
#ifndef HYDRA_HSEM_RCC
#define HYDRA_HSEM_RCC 0u        /* the semaphore that stands for "the clock tree is being set up" */
#endif
#ifndef HYDRA_RCC_WAIT_MS
#define HYDRA_RCC_WAIT_MS 200u   /* how long a core waits for the other one to set the tree up */
#endif

typedef enum {
    RCC_OWNER_CONFIGURE = 0,         /* this core sets the tree up */
    RCC_OWNER_SKIP_ALREADY_RUNNING,  /* the clock is on the PLL: touch nothing */
    RCC_OWNER_WAIT_FOR_OTHER_CORE    /* the other core holds the semaphore and the tree is not ready yet */
} RccOwnerDecision;

static inline RccOwnerDecision RccOwner_Decide(uint8_t semaphore_taken_by_me, uint8_t sysclk_is_pll)
{
    if (sysclk_is_pll) return RCC_OWNER_SKIP_ALREADY_RUNNING;
    return semaphore_taken_by_me ? RCC_OWNER_CONFIGURE : RCC_OWNER_WAIT_FOR_OTHER_CORE;
}

typedef enum { RCC_WAIT_KEEP_WAITING = 0, RCC_WAIT_DONE = 1, RCC_WAIT_TIMED_OUT = 2 } RccWaitState;

/* One look while waiting: the tree is ready, or the time is up, or keep waiting. Time is a difference of milliseconds, so it may wrap. */
static inline RccWaitState RccOwner_WaitStep(uint32_t waited_ms, uint8_t sysclk_is_pll)
{
    if (sysclk_is_pll) return RCC_WAIT_DONE;
    return waited_ms >= HYDRA_RCC_WAIT_MS ? RCC_WAIT_TIMED_OUT : RCC_WAIT_KEEP_WAITING;
}

#endif /* HYDRA_UMC_RCC_OWNER_H */
