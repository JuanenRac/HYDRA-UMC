/* =============================================================================
 * HYDRA-UMC Firmware - host logic tests for the explicit FDCAN kernel clock, the ENTER_BOOTLOADER hand-over and the health frame
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * fdcan_clock.h, app_boot_request.h and health_frame.h of the controller board. The test is compiled twice: with the three switches at their default
 * (0, the behaviour of the firmware as it ships) and with the three at 1 (what a build that asks for them runs). Evidence: simulator. The reset of
 * the board, the real kernel clock and the real error counters are bench rows 10, 14 and 19 of docs/BENCH_TESTS.md.
 */
#include <stdio.h>
#include <string.h>

#include "fdcan_clock.h"
#include "app_boot_request.h"
#include "health_frame.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static int failures;

static void test_fdcan_clock(void)
{
    const FdcanClockTree_t tree = FdcanClock_TreeAfterReset();
    /* the timing every init site sets: prescaler 1, segments 13 and 2 */
    TEST_ASSERT(FdcanClock_BitRate(16000000u, 1u, 13u, 2u) == 1000000u, "16 MHz with the firmware timing is 1 Mbit/s");
    TEST_ASSERT(FdcanClock_BitRate(0u, 1u, 13u, 2u) == 0u, "a clock that does not run gives no bit rate");
    TEST_ASSERT(FdcanClock_BitRate(16000000u, 0u, 13u, 2u) == 0u, "an empty prescaler gives no bit rate");
    /* the reason of the switch: the reset value of the selector is the HSE, and no HSE runs */
    TEST_ASSERT(FdcanClock_KernelHz(FDCAN_CLOCK_SRC_HSE, &tree) == 0u, "the reset selector feeds the peripheral with nothing");
    TEST_ASSERT(!FdcanClock_RateIsRight(FDCAN_CLOCK_SRC_HSE, &tree, 1u, 13u, 2u, 1000000u), "the reset selector does not give the stack bus rate");
    TEST_ASSERT(FdcanClock_KernelHz(FDCAN_CLOCK_SRC_PLLQ, &tree) == 0u, "no PLL runs");
    TEST_ASSERT(FdcanClock_KernelHz(3u, &tree) == 0u, "the reserved selector value feeds nothing");
    TEST_ASSERT(FDCAN_CLOCK_WANTED_SOURCE == FDCAN_CLOCK_SRC_PCLK1, "the firmware asks for the APB1 clock");
    TEST_ASSERT(FdcanClock_RateIsRight(FDCAN_CLOCK_WANTED_SOURCE, &tree, 1u, 13u, 2u, 1000000u), "the APB1 clock gives the stack bus rate");
    TEST_ASSERT(tree.pclk1_hz == FDCAN_CLOCK_ASSUMED_HZ, "the assumed clock is the one of the tree after reset");
    /* a different tree (a future PLL) breaks the assumption and the helper says so */
    FdcanClockTree_t fast = tree;
    fast.pclk1_hz = 64000000u;
    TEST_ASSERT(!FdcanClock_RateIsRight(FDCAN_CLOCK_SRC_PCLK1, &fast, 1u, 13u, 2u, 1000000u), "a faster APB1 clock needs a new timing");
    TEST_ASSERT(FdcanClock_RateIsRight(FDCAN_CLOCK_SRC_PCLK1, &fast, 4u, 13u, 2u, 1000000u), "and a prescaler of 4 gives the rate back");
}

static void test_app_boot_request(void)
{
    volatile uint32_t reg[2] = {0u, 0u};
    const uint8_t good[4] = {0xB0u, 0x07u, 0x1Du, 0x5Au};
    const uint8_t bad[4] = {0xB0u, 0x07u, 0x1Du, 0x5Bu};
#if HYDRA_APP_ENTER_BOOTLOADER
    TEST_ASSERT(AppBoot_OnEnterBootloader(reg, good, 4u, 1u) == APP_BOOT_ACCEPTED, "the exact magic with the drivers off is accepted");
    TEST_ASSERT(reg[0] == APP_BOOT_REQUEST_WORD && reg[1] == (uint32_t)APP_BOOT_REQUEST_COMPLEMENT, "the request word and its complement are written");
    TEST_ASSERT(AppBoot_TakeRequest(reg) == 1u, "the bootloader sees the request");
    TEST_ASSERT(reg[0] == 0u && reg[1] == 0u, "and clears it");
    TEST_ASSERT(AppBoot_TakeRequest(reg) == 0u, "so it applies once");
    TEST_ASSERT(AppBoot_OnEnterBootloader(reg, good, 4u, 0u) == APP_BOOT_REFUSED_MOVING, "a board whose drivers are enabled never resets for a frame");
    TEST_ASSERT(reg[0] == 0u, "and writes nothing");
    TEST_ASSERT(AppBoot_OnEnterBootloader(reg, bad, 4u, 1u) == APP_BOOT_REFUSED_BAD_MAGIC, "a wrong magic is refused");
    TEST_ASSERT(AppBoot_OnEnterBootloader(reg, good, 3u, 1u) == APP_BOOT_REFUSED_BAD_MAGIC, "a short payload is refused");
    TEST_ASSERT(AppBoot_OnEnterBootloader(reg, good, 5u, 1u) == APP_BOOT_REFUSED_BAD_MAGIC, "a long payload is refused");
    /* a register with random bits after a power-up is not a request */
    const uint32_t noise[][2] = {{0xFFFFFFFFu, 0xFFFFFFFFu}, {APP_BOOT_REQUEST_WORD, APP_BOOT_REQUEST_WORD}, {0u, (uint32_t)APP_BOOT_REQUEST_COMPLEMENT},
                                 {0x12345678u, 0x9ABCDEF0u}, {APP_BOOT_REQUEST_WORD, 0u}};
    for (unsigned i = 0; i < sizeof noise / sizeof noise[0]; i++) {
        reg[0] = noise[i][0];
        reg[1] = noise[i][1];
        TEST_ASSERT(AppBoot_TakeRequest(reg) == 0u, "noise in the register is not a request");
    }
#else
    TEST_ASSERT(AppBoot_OnEnterBootloader(reg, good, 4u, 1u) == APP_BOOT_IGNORED, "with the switch off the application ignores the command");
    TEST_ASSERT(reg[0] == 0u && reg[1] == 0u, "and writes nothing");
    reg[0] = APP_BOOT_REQUEST_WORD;
    reg[1] = (uint32_t)APP_BOOT_REQUEST_COMPLEMENT;
    TEST_ASSERT(AppBoot_TakeRequest(reg) == 0u, "with the switch off the bootloader reads no request");
    (void)bad;
#endif
}

/* The bytes of the vectors are the ones tools/health_frame.py builds (tests/test_health_frame.py compares them). */
static void test_health_frame(void)
{
    HealthFrame_t h;
    uint8_t out[8];
    HealthFrame_Reset(&h);
#if HYDRA_HEALTH_FRAME
    h.flags = HEALTH_FLAG_ESTOP | HEALTH_FLAG_BUSOFF;
    h.tx_errors = 3u; h.rx_errors = 7u; h.stalls = 1u;
    TEST_ASSERT(HealthFrame_Due(&h, 100u, out) == 1u, "the first frame goes out at once");
    /* VECTOR: 48 00 03 03 07 01 00 EB */
    TEST_ASSERT(out[0] == 0x48u && out[1] == 0x00u && out[2] == 0x03u && out[3] == 0x03u && out[4] == 0x07u && out[5] == 0x01u && out[6] == 0x00u,
                "the first frame has the documented bytes");
    TEST_ASSERT(HealthFrame_Check(out) == out[7], "the check byte matches");
    TEST_ASSERT(out[7] == 0xEBu, "the check of the first frame is 0xA5 xor the seven bytes");
    TEST_ASSERT((h.flags & HEALTH_FLAG_BUSOFF) == 0u, "bus-off is reported once");
    TEST_ASSERT(HealthFrame_Due(&h, 600u, out) == 0u, "no second frame within the period");
    TEST_ASSERT(HealthFrame_Due(&h, 1100u, out) == 1u, "one per period");
    TEST_ASSERT(out[1] == 1u && (out[2] & HEALTH_FLAG_BUSOFF) == 0u, "the sequence counts and bus-off is gone");
    /* a timer that wraps */
    HealthFrame_Reset(&h);
    TEST_ASSERT(HealthFrame_Due(&h, 0xFFFFFF00u, out) == 1u, "first frame near the wrap");
    TEST_ASSERT(HealthFrame_Due(&h, 0x00000100u, out) == 0u, "the wrap does not count as a full period (0x200 ms)");
    TEST_ASSERT(HealthFrame_Due(&h, 0x000003F0u, out) == 1u, "after the period across the wrap");
    /* uptime saturates */
    HealthFrame_Reset(&h);
    h.uptime_ms = 16000u * 300u;
    HealthFrame_Build(&h, out);
    TEST_ASSERT(out[6] == 255u, "the uptime saturates at 255 units");
#else
    TEST_ASSERT(HealthFrame_Due(&h, 100u, out) == 0u, "with the switch off no frame is ever due");
    TEST_ASSERT(HealthFrame_Due(&h, 5000u, out) == 0u, "still none later");
    h.flags = 0x01u; h.tx_errors = 3u; h.rx_errors = 7u; h.stalls = 1u;
    HealthFrame_Build(&h, out);
    TEST_ASSERT(out[0] == 0x48u && out[1] == 0x00u && out[2] == 0x01u && out[3] == 0x03u && out[4] == 0x07u && out[5] == 0x01u && out[6] == 0x00u,
                "the builder itself still gives the documented bytes");
    TEST_ASSERT(HealthFrame_Check(out) == out[7], "and its check byte");
#endif
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - fdcan_clock.h, app_boot_request.h, health_frame.h (switches %d%d%d)\n",
           HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT, HYDRA_APP_ENTER_BOOTLOADER, HYDRA_HEALTH_FRAME);
    test_fdcan_clock();
    test_app_boot_request();
    test_health_frame();
    if (failures) printf("%d FAILED\n", failures);
    else printf("ok\n");
    return failures ? 1 : 0;
}
