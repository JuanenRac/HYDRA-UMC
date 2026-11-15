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
// A digest that cannot be read leaves `out` zero, which no signature covers and no metadata matches.
#define BOOT_STAGE_DIGEST(prefix, prefix_len, total, out) ((void)BootNor_Digest((prefix), (prefix_len), (total), (out)))
#define BOOT_STAGE_WORD(offset) BootNor_Word(offset)
#define BOOT_STAGE_FINISH(total, crc32) BootNor_Finish((total), (crc32))
#define BOOT_STAGE_COMMIT(total, crc32) BootNor_Commit((total), (crc32))

#endif // BOOT_STAGE_NOR_H
