/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/HostLinkSpi3.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Driver of the host link on the H757 board: SPI3 as master (SCK PB3, MISO PB4, MOSI PB5, chip select PA15) towards the Jetson, under the transport of
 * Common/host_link_master.h (docs/HOST_LINK_MASTER.md). OFF by default: with HYDRA_HOST_LINK_MASTER 0 (as it ships) HostLinkSpi3.c compiles to an empty unit and the pins
 * are not touched. The pins are shared with JTAG, so only SWD can be used on this board. Not run on a board: the logic analyser test of docs/HOST_LINK_MASTER.md remains.
 * ========================================================================== */
#ifndef HYDRA_UMC_HOST_LINK_SPI3_H
#define HYDRA_UMC_HOST_LINK_SPI3_H

#ifndef HYDRA_HOST_LINK_MASTER
#define HYDRA_HOST_LINK_MASTER 0
#endif

#if HYDRA_HOST_LINK_MASTER

#include "host_link_master.h"

/* Starts SPI3 and the transport. `dispatch` runs a request of the host (see HostLinkOps). Returns 1 when the peripheral and the configuration were accepted; on 0 the link
 * does not run and the rest of the application is not affected. */
int HostLinkSpi3_Init(const HostLinkConfig *cfg, int (*dispatch)(void *ctx, const uint8_t *request, uint8_t *answer12), void *ctx);

/* One step of the transport; call it from a task every millisecond or so. Never blocks for more than one frame time. */
HostLinkPoll HostLinkSpi3_Poll(uint32_t now_ms);

HostLink_t *HostLinkSpi3_Instance(void);

#endif /* HYDRA_HOST_LINK_MASTER */

#endif /* HYDRA_UMC_HOST_LINK_SPI3_H */
