/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/panel_init_table.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The start-up table of the display panel, made from config/panel.cfg by tools/dsi_plan.py (python tools/dsi_plan.py --write). Do not edit it by hand: tests/test_dsi_plan.py
 * fails when it is not the table of the file.
 * ============================================================================= */
#ifndef HYDRA_UMC_PANEL_INIT_TABLE_H
#define HYDRA_UMC_PANEL_INIT_TABLE_H

#include "panel_start.h"

#define PANEL_INIT_RESET_LOW_MS 10u
#define PANEL_INIT_RESET_HIGH_MS 120u


static const PanelStep g_panel_init[] = {
    {0x11, 0, NULL, 120},
    {0x29, 0, NULL, 20},
};

#define PANEL_INIT_STEPS 2u

#endif /* HYDRA_UMC_PANEL_INIT_TABLE_H */
