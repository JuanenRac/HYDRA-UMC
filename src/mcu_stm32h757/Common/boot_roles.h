// =============================================================================
// HYDRA-UMC Kinematic Brain - which role the bootloader of this core plays on the platform it is built for
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The two bootloaders of the dual-core chip have two roles. The GATEWAY owns the host link (SPI1), the stack bus (FDCAN1) and the I2C4, takes every update that arrives from outside and
// relays what is meant for the other core over the mailbox (ipc_link.h). The BACKEND owns none of those and only answers what the gateway relays. The platform profile says which core
// owns SPI1, FDCAN1 and I2C4: the CM4 on the H745 platform (what the bench boards run today) and the CM7 on the H757 board. This header turns that into three facts for the core that is
// being compiled (CORE_CM4 or CORE_CM7, set by the compiler flags of the build), so that a source can say "this is the gateway" with a macro and the build for the default platform keeps
// the code it has today:
//
//   HYDRA_BOOT_ROLE_GATEWAY / HYDRA_BOOT_ROLE_BACKEND   exactly one of them is 1
//   HYDRA_BOOT_THIS_CORE                                HYDRA_OWNER_CM4 or HYDRA_OWNER_CM7
//   HYDRA_BOOT_OTHER_CORE                               the core on the other side of the mailbox
//
// Nothing else is in this header, so that including it changes no object code. Nothing here has run on a board.
// =============================================================================
#ifndef HYDRA_BOOT_ROLES_H
#define HYDRA_BOOT_ROLES_H

#include "platform_profile.h"

#if defined(CORE_CM7) && defined(CORE_CM4)
#error "build for one core: CORE_CM4 or CORE_CM7, not both"
#elif defined(CORE_CM7)
#define HYDRA_BOOT_THIS_CORE HYDRA_OWNER_CM7
#define HYDRA_BOOT_OTHER_CORE HYDRA_OWNER_CM4
#elif defined(CORE_CM4)
#define HYDRA_BOOT_THIS_CORE HYDRA_OWNER_CM4
#define HYDRA_BOOT_OTHER_CORE HYDRA_OWNER_CM7
#else
#error "build for one core: define CORE_CM4 or CORE_CM7"
#endif

// One core owns the three peripherals of the host side; the platform profile cannot split them between the cores without this file being changed on purpose.
_Static_assert(HYDRA_OWNER_HOST_SPI == HYDRA_OWNER_FDCAN1 && HYDRA_OWNER_FDCAN1 == HYDRA_OWNER_I2C4, "one core owns the host SPI, FDCAN1 and I2C4");

#define HYDRA_BOOT_ROLE_GATEWAY (HYDRA_BOOT_THIS_CORE == HYDRA_OWNER_HOST_SPI ? 1 : 0)
#define HYDRA_BOOT_ROLE_BACKEND (HYDRA_BOOT_THIS_CORE == HYDRA_OWNER_HOST_SPI ? 0 : 1)

#endif // HYDRA_BOOT_ROLES_H
