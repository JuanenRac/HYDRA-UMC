/* Host test of the clock tree of the H757 platform: the header refuses to compile when the profile does not add up (see clock_tree.h), and these checks
 * recompute the same numbers at run time and look at the values the register code will use. Build with -DHYDRA_PLATFORM_H757. */
#include <stdio.h>
#include "clock_tree.h"

#if !defined(HYDRA_PLATFORM_H757)
#error "this test is for the H757 platform: build it with -DHYDRA_PLATFORM_H757"
#endif

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
    /* the crystal, the PLL values and what they give */
    CHECK(HYDRA_CLOCK_HSE_HZ == 25000000UL);
    CHECK(HYDRA_PLL1_VCO_HZ == 800000000UL);   /* 25 MHz / 5 * 160 */
    CHECK(HYDRA_CLOCK_SYSCLK_HZ == 400000000UL);
    CHECK(HYDRA_CLOCK_HCLK_HZ == 200000000UL);
    CHECK(HYDRA_PLL2_VCO_HZ == 400000000UL);   /* 25 MHz / 5 * 80 */
    CHECK(HYDRA_CLOCK_PLL2_Q_HZ == 100000000UL);
    /* the stack bus: 1 Mbit/s with prescaler 5 at the 100 MHz FDCAN kernel clock */
    CHECK(HYDRA_STACK_BUS_PRESCALER == 5U);
    CHECK(HYDRA_CLOCK_KERNEL_FDCAN_HZ == 100000000UL);
    CHECK(HYDRA_CLOCK_KERNEL_FDCAN_HZ / (HYDRA_STACK_BUS_PRESCALER * (1U + HYDRA_STACK_BUS_TIME_SEG1 + HYDRA_STACK_BUS_TIME_SEG2)) == 1000000UL);
    /* sample point of the nominal bit: (1 + seg1) / (1 + seg1 + seg2), between 75 % and 90 % */
    CHECK(100U * (1U + HYDRA_STACK_BUS_TIME_SEG1) >= 75U * (1U + HYDRA_STACK_BUS_TIME_SEG1 + HYDRA_STACK_BUS_TIME_SEG2));
    CHECK(100U * (1U + HYDRA_STACK_BUS_TIME_SEG1) <= 90U * (1U + HYDRA_STACK_BUS_TIME_SEG1 + HYDRA_STACK_BUS_TIME_SEG2));
    /* the owner of the stack-bus peripherals is the CM7 on this platform */
    CHECK(HYDRA_OWNER_HOST_SPI == HYDRA_OWNER_CM7);
    CHECK(HYDRA_OWNER_FDCAN1 == HYDRA_OWNER_CM7);
    CHECK(HYDRA_OWNER_I2C4 == HYDRA_OWNER_CM7);
    CHECK(HYDRA_SPI_ROLE == HYDRA_SPI_ROLE_MASTER);

    if (failures == 0) printf("CLOCK_TREE=PASS\n");
    return failures == 0 ? 0 : 1;
}
