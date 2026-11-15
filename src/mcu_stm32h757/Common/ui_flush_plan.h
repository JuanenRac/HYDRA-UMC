/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/ui_flush_plan.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * ui_flush_plan.h - how the interface library draws on the picture of the panel (HAL-free, host-testable, independent of the library). The library renders into a small buffer, a
 * band of whole lines of the picture, and asks for each finished area to be copied into the frame buffer. This file works out how many lines fit the render buffer that the file
 * allows (config/ui_flush.cfg: a size in KiB and one or two buffers; the interface budget of docs/UI_STACK_H757.md holds a single buffer of a tenth of the frame), the strips the
 * picture is cut in, and, for an area in the inclusive coordinates that the library uses (x1, y1, x2, y2), the DMA2D copy job of Common/display_plan.h that puts the buffer, whose
 * lines are as long as the area is wide, at its place in the frame buffer. An area that is inverted, negative or outside the picture is refused. The library itself, the tick and
 * the touch input are not part of this file. tools/ui_flush_plan.py is the Python twin. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_UI_FLUSH_PLAN_H
#define HYDRA_UMC_UI_FLUSH_PLAN_H

#include <stdint.h>

#include "display_plan.h"

#define UFP_BUFFER_KB_MIN 4UL
#define UFP_BUFFER_KB_MAX 2048UL
#define UFP_BUFFERS_MAX 2UL

#define UFP_OK 0
#define UFP_E_ARG 1
#define UFP_E_BUFFER 2 /* the size is outside 4..2048 KiB, or the number of buffers is not 1 or 2 */
#define UFP_E_PANEL 3  /* the panel has no size or no known format */
#define UFP_E_SMALL 4  /* the buffer does not hold a single line */
#define UFP_E_INDEX 5  /* a strip that does not exist */
#define UFP_E_AREA 6   /* an area that is inverted, negative or outside the picture */
#define UFP_E_JOB 7    /* the copy job was refused (a source address that is not word aligned) */

typedef struct {
    uint32_t buffer_kb;
    uint32_t buffers;
} UfpConfig;

typedef struct {
    uint32_t lines;        /* whole lines of the picture in one render buffer */
    uint32_t buffer_bytes; /* the bytes of one render buffer */
    uint32_t buffers;
    uint32_t strips;       /* the bands the picture is cut in */
    uint32_t total_bytes;  /* all the render buffers */
} UfpPlan;

typedef struct {
    int32_t x1, y1, x2, y2; /* inclusive, as the interface library gives them */
} UfpArea;

static inline int UfpPlan_Make(const DisplayPanel *panel, const UfpConfig *config, UfpPlan *plan)
{
    if (!panel || !config || !plan) return UFP_E_ARG;
    if (config->buffer_kb < UFP_BUFFER_KB_MIN || config->buffer_kb > UFP_BUFFER_KB_MAX || config->buffers < 1u || config->buffers > UFP_BUFFERS_MAX) return UFP_E_BUFFER;
    if (panel->width == 0u || panel->height == 0u || (panel->format != DISPLAY_FMT_RGB565 && panel->format != DISPLAY_FMT_ARGB8888)) return UFP_E_PANEL;
    const uint32_t line_bytes = (uint32_t)panel->width * Display_BytesPerPixel(panel->format);
    uint32_t lines = (config->buffer_kb * 1024u) / line_bytes;
    if (lines < 1u) return UFP_E_SMALL;
    if (lines > panel->height) lines = panel->height;
    plan->lines = lines;
    plan->buffer_bytes = lines * line_bytes;
    plan->buffers = config->buffers;
    plan->strips = ((uint32_t)panel->height + lines - 1u) / lines;
    plan->total_bytes = plan->buffer_bytes * config->buffers;
    return UFP_OK;
}

/* The rectangle of strip `index` (the last one is the short one). */
static inline int UfpPlan_Strip(const DisplayPanel *panel, const UfpPlan *plan, uint32_t index, DisplayRect *r)
{
    if (!panel || !plan || !r) return UFP_E_ARG;
    if (index >= plan->strips) return UFP_E_INDEX;
    const uint32_t y = index * plan->lines;
    const uint32_t rest = (uint32_t)panel->height - y;
    r->x = 0u;
    r->y = (uint16_t)y;
    r->w = panel->width;
    r->h = (uint16_t)(rest < plan->lines ? rest : plan->lines);
    return UFP_OK;
}

/* The rectangle of an area of the library. */
static inline int UfpPlan_Rect(const DisplayPanel *panel, const UfpArea *area, DisplayRect *r)
{
    if (!panel || !area || !r) return UFP_E_ARG;
    if (area->x1 < 0 || area->y1 < 0 || area->x2 < area->x1 || area->y2 < area->y1 || area->x2 >= (int32_t)panel->width || area->y2 >= (int32_t)panel->height) return UFP_E_AREA;
    r->x = (uint16_t)area->x1;
    r->y = (uint16_t)area->y1;
    r->w = (uint16_t)(area->x2 - area->x1 + 1);
    r->h = (uint16_t)(area->y2 - area->y1 + 1);
    return UFP_OK;
}

/* The copy of the render buffer at `src_addr` (lines as long as the area is wide) into the frame buffer. */
static inline int UfpPlan_FlushJob(const DisplayPlan *plan, const DisplayPanel *panel, const UfpArea *area, uint32_t src_addr, Dma2dJob *job)
{
    DisplayRect r;
    const int rc = UfpPlan_Rect(panel, area, &r);
    if (rc != UFP_OK) return rc;
    return Display_CopyJob(plan, panel, &r, src_addr, r.w, job) == DISPLAY_OK ? UFP_OK : UFP_E_JOB;
}

#endif /* HYDRA_UMC_UI_FLUSH_PLAN_H */
