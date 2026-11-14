/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/DisplayLtdc.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Driver of the LTDC (one layer on the frame buffer in the SDRAM) and of the DMA2D (fills and copies) of the H757 board, under the numbers of Common/display_plan.h. OFF by
 * default: with HYDRA_DISPLAY_LTDC 0 (as it ships) DisplayLtdc.c compiles to an empty unit and no register is touched. The panel (size, porches, format), the frame buffer and the
 * poll limit are not constants: the caller passes them from config/display.cfg. The DSI host and the start-up sequence of the panel are not here (they stay for the bench). Not run
 * on a board: the picture on the real panel remains to be seen.
 * ========================================================================== */
#ifndef HYDRA_UMC_DISPLAY_LTDC_H
#define HYDRA_UMC_DISPLAY_LTDC_H

#ifndef HYDRA_DISPLAY_LTDC
#define HYDRA_DISPLAY_LTDC 0
#endif

#if HYDRA_DISPLAY_LTDC

#include <stdint.h>
#include "display_plan.h"

typedef struct {
    DisplayPanel panel;
    uint32_t fb_addr;                 /* the frame buffer in the SDRAM (word aligned) */
    uint32_t fb_window_bytes;         /* the memory that belongs to it */
    uint32_t poll_limit;              /* how many times a DMA2D job is polled before it is given up (1..1000000) */
    void (*relax)(void);              /* called between two polls (an RTOS yield, a pause); may be NULL */
} DisplayLtdcConfig;

typedef enum {
    DISPLAYLTDC_OK = 0,
    DISPLAYLTDC_ERR_NOT_STARTED,   /* Init was not called or did not succeed */
    DISPLAYLTDC_ERR_BAD_REQUEST,   /* the rectangle or the address of the job is refused */
    DISPLAYLTDC_ERR_BUSY,          /* the previous job did not finish */
    DISPLAYLTDC_ERR_TRANSFER,      /* the DMA2D reported a transfer or configuration error */
    DISPLAYLTDC_ERR_TIMEOUT        /* the job did not finish within the poll limit */
} DisplayLtdcStatus;

/* Works out the plan, starts the clocks, writes the timing and the layer and turns the LTDC on (the layer shows the frame buffer as it is). Returns 1 when the configuration was
 * accepted; on 0 no register was written. */
int DisplayLtdc_Init(const DisplayLtdcConfig *cfg);

/* Fills a rectangle with a colour of the format of the picture, and copies a bitmap of the same format to a rectangle; each waits for the job to end. */
DisplayLtdcStatus DisplayLtdc_Fill(const DisplayRect *rect, uint32_t color);
DisplayLtdcStatus DisplayLtdc_Copy(const DisplayRect *rect, uint32_t src_addr, uint16_t src_pitch_px);

/* The plan in use (NULL before a successful Init). */
const DisplayPlan *DisplayLtdc_Plan(void);

#endif /* HYDRA_DISPLAY_LTDC */

#endif /* HYDRA_UMC_DISPLAY_LTDC_H */
