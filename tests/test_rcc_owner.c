/* =============================================================================
 * HYDRA-UMC Firmware - host logic tests for rcc_owner.h (which core of the H745 sets the clock tree up)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Two cores run the start-up of rcc_owner.h against a model of the hardware semaphore and of the clock source. Every interleaving of their steps is
 * tried (the scheduler of the test picks, at each step, which core moves), and in none of them is the tree set up twice. Evidence: simulator; the
 * real semaphore and the real clock are bench row 8 of docs/BENCH_TESTS.md.
 */
#include <stdio.h>
#include <string.h>

#include "rcc_owner.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static int failures;

typedef struct {
    int sem_owner;       /* -1 free, else the core that holds the semaphore */
    uint8_t on_pll;
    int configure_count; /* how many times a core started setting the tree up */
} Hardware;

typedef struct {
    int id;
    int step;            /* 0 take, 1 read the clock, 2 decide, 3 begin configuring, 4 finish configuring, 5 waiting, 6 done */
    uint8_t mine;
    uint8_t seen_pll;
    uint32_t waited_ms;
    int timed_out;
    int configured;
} Core;

#define WAIT_STEP_MS 50u

static void advance(Hardware *hw, Core *core)
{
    switch (core->step) {
    case 0:
        if (hw->sem_owner < 0 || hw->sem_owner == core->id) { hw->sem_owner = core->id; core->mine = 1; }
        core->step = 1;
        break;
    case 1:
        core->seen_pll = hw->on_pll;
        core->step = 2;
        break;
    case 2: {
        const RccOwnerDecision d = RccOwner_Decide(core->mine, core->seen_pll);
        core->step = d == RCC_OWNER_CONFIGURE ? 3 : (d == RCC_OWNER_WAIT_FOR_OTHER_CORE ? 5 : 6);
        break;
    }
    case 3:
        hw->configure_count++;
        core->configured = 1;
        core->step = 4;
        break;
    case 4:
        hw->on_pll = 1;
        core->step = 6;
        break;
    case 5: {
        const RccWaitState w = RccOwner_WaitStep(core->waited_ms, hw->on_pll);
        if (w == RCC_WAIT_DONE) core->step = 6;
        else if (w == RCC_WAIT_TIMED_OUT) { core->timed_out = 1; core->step = 6; }
        else core->waited_ms += WAIT_STEP_MS;
        break;
    }
    default:
        break;
    }
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - rcc_owner.h\n");

    /* the decision table */
    TEST_ASSERT(RccOwner_Decide(1, 0) == RCC_OWNER_CONFIGURE, "the core that has the semaphore and finds the clock on the reset default sets it up");
    TEST_ASSERT(RccOwner_Decide(0, 0) == RCC_OWNER_WAIT_FOR_OTHER_CORE, "a core that lost the semaphore waits");
    TEST_ASSERT(RccOwner_Decide(0, 1) == RCC_OWNER_SKIP_ALREADY_RUNNING, "a core that finds the clock running touches nothing");
    TEST_ASSERT(RccOwner_Decide(1, 1) == RCC_OWNER_SKIP_ALREADY_RUNNING, "a core that restarted alone (its semaphore is still held) touches nothing");
    TEST_ASSERT(RccOwner_WaitStep(0, 1) == RCC_WAIT_DONE, "the wait ends as soon as the clock is ready");
    TEST_ASSERT(RccOwner_WaitStep(HYDRA_RCC_WAIT_MS - 1u, 0) == RCC_WAIT_KEEP_WAITING, "the wait goes on before the limit");
    TEST_ASSERT(RccOwner_WaitStep(HYDRA_RCC_WAIT_MS, 0) == RCC_WAIT_TIMED_OUT, "and ends at the limit");
    TEST_ASSERT(HYDRA_RCC_HSEM_HANDSHAKE == 0, "the switch ships off");

    /* every interleaving of two cores, 14 steps each is more than they need: 2^14 sequences */
    int worst_configure = 0, runs = 0, nobody_configured = 0, timeouts = 0;
    for (unsigned order = 0; order < (1u << 14); order++) {
        Hardware hw = {-1, 0, 0};
        Core cores[2];
        memset(cores, 0, sizeof cores);
        cores[0].id = 0;
        cores[1].id = 1;
        for (int i = 0; i < 14; i++) advance(&hw, &cores[(order >> i) & 1u]);
        for (int i = 0; i < 40; i++) { advance(&hw, &cores[0]); advance(&hw, &cores[1]); }   /* then both run to the end */
        runs++;
        if (hw.configure_count > worst_configure) worst_configure = hw.configure_count;
        if (hw.configure_count == 0) nobody_configured++;
        timeouts += cores[0].timed_out + cores[1].timed_out;
    }
    TEST_ASSERT(runs == (1 << 14), "all interleavings ran");
    TEST_ASSERT(worst_configure == 1, "in no interleaving is the tree set up twice");
    TEST_ASSERT(nobody_configured == 0, "and in every one somebody sets it up");
    (void)timeouts;

    /* one core restarts alone after the tree was set up: it must not configure again */
    {
        Hardware hw = {-1, 0, 0};
        Core a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.id = 0; b.id = 1;
        for (int i = 0; i < 10; i++) { advance(&hw, &a); advance(&hw, &b); }
        TEST_ASSERT(hw.configure_count == 1 && hw.on_pll == 1, "first start: one core set the tree up");
        const int owner = hw.sem_owner;
        Core restarted;
        memset(&restarted, 0, sizeof restarted);
        restarted.id = owner;                       /* the owner restarts and takes the semaphore it still holds */
        for (int i = 0; i < 10; i++) advance(&hw, &restarted);
        TEST_ASSERT(hw.configure_count == 1, "a core that restarts alone does not set the tree up again");
    }
    /* the other core is dead: the waiting core gives up after the limit */
    {
        Hardware hw = {1, 0, 0};                    /* held by a core that never moves */
        Core c;
        memset(&c, 0, sizeof c);
        c.id = 0;
        for (int i = 0; i < 100; i++) advance(&hw, &c);
        TEST_ASSERT(c.timed_out == 1 && hw.configure_count == 0, "a core waiting for a dead core carries on after the time-out without configuring");
    }

    if (failures) printf("%d FAILED\n", failures);
    else printf("ok\n");
    return failures ? 1 : 0;
}
