/* =============================================================================
 * HYDRA-UMC Firmware - host test of boot_roles.h (the role of the bootloader of each core on each platform)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Built four times: for the CM4 and for the CM7, on the default platform and with -DHYDRA_PLATFORM_H757. The expected role is written here for each of the four cases, so a change
 * of the owner in the platform profile must change this test on purpose. Nothing here touches a peripheral.
 */
#include <stdio.h>
#include "boot_roles.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

int main(void)
{
    CHECK(HYDRA_BOOT_ROLE_GATEWAY + HYDRA_BOOT_ROLE_BACKEND == 1, "exactly one role");
    CHECK(HYDRA_BOOT_THIS_CORE != HYDRA_BOOT_OTHER_CORE, "the two cores are different");

#if defined(CORE_CM4)
    CHECK(HYDRA_BOOT_THIS_CORE == HYDRA_OWNER_CM4 && HYDRA_BOOT_OTHER_CORE == HYDRA_OWNER_CM7, "the CM4 build knows it is the CM4");
#if defined(HYDRA_PLATFORM_H757)
    CHECK(HYDRA_BOOT_ROLE_BACKEND == 1 && HYDRA_BOOT_ROLE_GATEWAY == 0, "H757 platform: the CM4 is the backend");
#else
    CHECK(HYDRA_BOOT_ROLE_GATEWAY == 1 && HYDRA_BOOT_ROLE_BACKEND == 0, "default platform: the CM4 is the gateway, as today");
#endif
#else
    CHECK(HYDRA_BOOT_THIS_CORE == HYDRA_OWNER_CM7 && HYDRA_BOOT_OTHER_CORE == HYDRA_OWNER_CM4, "the CM7 build knows it is the CM7");
#if defined(HYDRA_PLATFORM_H757)
    CHECK(HYDRA_BOOT_ROLE_GATEWAY == 1 && HYDRA_BOOT_ROLE_BACKEND == 0, "H757 platform: the CM7 is the gateway");
#else
    CHECK(HYDRA_BOOT_ROLE_BACKEND == 1 && HYDRA_BOOT_ROLE_GATEWAY == 0, "default platform: the CM7 is the backend, as today");
#endif
#endif

    if (failures == 0) printf("BOOT_ROLES=PASS\n");
    return failures == 0 ? 0 : 1;
}
