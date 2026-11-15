// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM7) - boot_stage_nor.h: the staging of an update in the NOR flash
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// Included by boot_update_core_impl.h (and so by bootloader_protocol.c) only when HYDRA_NOR_BACKUP is 1: the update is then staged page by page in the NOR (bootloader_nor.c) instead of in the
// backup slot of the internal flash, which with the 768 KB slot is part of the application. The NOR erases what the size needs, so the number of pages is not used.
// =============================================================================
#ifndef BOOT_STAGE_NOR_H
#define BOOT_STAGE_NOR_H

#include "bootloader_nor.h"

#define BOOT_STAGE_ERASE(total, pages) ((void)(pages), BootNor_Begin(total))
#define BOOT_STAGE_PAGE(address, index, buffer, length) ((void)(address), BootNor_Page((index), (buffer), (length)))

#endif // BOOT_STAGE_NOR_H
