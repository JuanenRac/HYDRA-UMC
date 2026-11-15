/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/DsiHost.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The DSI host of the H757 on the CM7, the link between the LTDC (CM7/DisplayLtdc.c) and the display panel: it starts the host with the PLL divisors, the lanes and the video times
 * that Common/dsi_plan.h worked out from config/panel.cfg and config/display.cfg (video mode with sync pulses, the pixel packets of the width of the picture, every low-power option
 * of the blanking periods on), and gives the panel start-up of Common/panel_start.h the two operations it needs from the host: a DCS write (a short write for 0 or 1 parameter, a long
 * write above that) and a wait. The reset line of the panel is not driven here: its pin is not in the platform profile (the lines next to the touch controller belong to the touch
 * controller), so the caller hands panel_start its own reset operation. OFF by default: with HYDRA_DSI_HOST 0 (as it ships) DsiHost.c compiles to an empty unit and nothing is
 * touched. The values are candidates (CubeMX, unconfirmed). Not run on a board: the picture on the real panel with the vendor init table is the check that remains.
 * ========================================================================== */
#ifndef HYDRA_UMC_DSI_HOST_H
#define HYDRA_UMC_DSI_HOST_H

#ifndef HYDRA_DSI_HOST
#define HYDRA_DSI_HOST 0
#endif

#if HYDRA_DSI_HOST

#include <stdint.h>

#include "stm32h7xx_hal.h"
#include "dsi_plan.h"

#define DSIH_OK 0
#define DSIH_E_ARG 1   /* the configuration or the plan was refused */
#define DSIH_E_HAL 2   /* the HAL refused the PLL, the video mode or the start */
#define DSIH_E_STATE 3 /* already started */

/* Starts the host for the picture of `config` with the divisors of `plan` (from DsiPlan_Make for the same configuration). */
int DsiHost_Open(const DsipConfig *config, const DsipPlan *plan);

/* The operations of Common/panel_start.h (PanelOps.dcs_write and PanelOps.delay_ms). `ctx` is not used. A write before the host is started fails (returns 1). */
int DsiHost_DcsWrite(void *ctx, uint8_t command, const uint8_t *params, uint8_t count);
void DsiHost_Delay(void *ctx, uint32_t ms);

#endif /* HYDRA_DSI_HOST */

#endif /* HYDRA_UMC_DSI_HOST_H */
