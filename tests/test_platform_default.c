/* Host test of the choice of the default platform (platform_profile.h): a build that names no platform gets the H745 of the bench unless HYDRA_DEFAULT_PLATFORM_H757 is 1, a platform named on
 * the command line always wins, and the numbers that follow are those of the platform that was chosen. Build with -DEXPECT_PLATFORM=745 or -DEXPECT_PLATFORM=757 and the definitions to try. */
#include <stdio.h>
#include "platform_profile.h"

#if !defined(EXPECT_PLATFORM)
#error "build this test with -DEXPECT_PLATFORM=745 or -DEXPECT_PLATFORM=757"
#endif

#if defined(HYDRA_PLATFORM_H757) && defined(HYDRA_PLATFORM_H745)
#error "both platforms are defined"
#endif
#if defined(HYDRA_PLATFORM_H757)
static int defined_h757(void) { return 1; }
#else
static int defined_h757(void) { return 0; }
#endif

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
#if EXPECT_PLATFORM == 757
    CHECK(defined_h757() == 1);
    CHECK(HYDRA_SPI_ROLE == HYDRA_SPI_ROLE_MASTER);
    CHECK(HYDRA_OWNER_FDCAN1 == HYDRA_OWNER_CM7);
    CHECK(HYDRA_HOST_SPI_INSTANCE == 3);
    CHECK(HYDRA_STACK_BUS_KERNEL_CLOCK_HZ == 100000000UL);
    CHECK(HYDRA_STACK_BUS_PRESCALER == 5U);
    CHECK(HYDRA_CLOCK_SPI5_SEL == 0UL);
#else
    CHECK(defined_h757() == 0);
    CHECK(HYDRA_SPI_ROLE == HYDRA_SPI_ROLE_SLAVE);
    CHECK(HYDRA_OWNER_FDCAN1 == HYDRA_OWNER_CM4);
    CHECK(HYDRA_HOST_SPI_INSTANCE == 1);
    CHECK(HYDRA_STACK_BUS_KERNEL_CLOCK_HZ == 16000000UL);
    CHECK(HYDRA_STACK_BUS_PRESCALER == 1U);
#endif
    if (failures == 0) printf("PLATFORM_DEFAULT=PASS\n");
    return failures == 0 ? 0 : 1;
}
