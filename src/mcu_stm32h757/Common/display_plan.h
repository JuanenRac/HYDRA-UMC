/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/display_plan.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * display_plan.h - the numbers that the LTDC and the DMA2D of the H757 board need for the panel, worked out without touching a register (HAL-free, host-testable). From the size of
 * the picture, the sync, back and front porch of both axes and the place of the frame buffer in the SDRAM it makes the four timing words of the LTDC (SSCR, BPCR, AWCR, TWCR), the
 * window words and the line length of the layer, and it refuses a panel whose frame buffer does not fit the window it was given. It also makes the register words of a DMA2D fill
 * (register to memory) and of a DMA2D copy (memory to memory, same format). The timings are starting values for the bench (config/display.cfg), not measurements of a panel. The
 * driver that writes them is src/mcu_stm32h757/CM7/DisplayLtdc.c; tests/test_display_plan.c runs this file on the host. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_DISPLAY_PLAN_H
#define HYDRA_UMC_DISPLAY_PLAN_H

#include <stdint.h>
#include <string.h>

#define DISPLAY_FMT_RGB565 0u
#define DISPLAY_FMT_ARGB8888 1u

#define DISPLAY_MAX_WIDTH 2048u
#define DISPLAY_MAX_HEIGHT 2048u
#define DISPLAY_MAX_SYNC 1024u /* the limit of every sync, back porch and front porch */

/* the register values of the LTDC pixel format (PFCR) and of the DMA2D colour mode (OPFCCR), both for the two formats above */
#define DISPLAY_LTDC_PF_ARGB8888 0u
#define DISPLAY_LTDC_PF_RGB565 2u
#define DISPLAY_DMA2D_CM_ARGB8888 0u
#define DISPLAY_DMA2D_CM_RGB565 2u

/* the mode of DMA2D_CR (bits 17:16) and its start bit */
#define DISPLAY_DMA2D_CR_START 0x00000001UL
#define DISPLAY_DMA2D_CR_M2M 0x00000000UL
#define DISPLAY_DMA2D_CR_R2M 0x00030000UL

typedef struct {
    uint16_t width, height;
    uint16_t hsync, hbp, hfp; /* horizontal sync width, back porch and front porch, in pixel clocks */
    uint16_t vsync, vbp, vfp; /* the same on the vertical axis, in lines */
    uint8_t format;           /* DISPLAY_FMT_* */
} DisplayPanel;

typedef struct {
    uint32_t sscr, bpcr, awcr, twcr;    /* the LTDC timing words */
    uint32_t whpcr, wvpcr;              /* the horizontal and vertical window of the layer */
    uint32_t pfcr, cfblr, cfblnr;       /* pixel format, line length and pitch, number of lines */
    uint32_t cfbar;                     /* the address of the frame buffer */
    uint32_t pitch_bytes, frame_bytes;
    uint32_t dma2d_cm;                  /* the DMA2D colour mode of the same format */
} DisplayPlan;

typedef enum {
    DISPLAY_OK = 0,
    DISPLAY_ERR_ARGUMENT,   /* a missing pointer or a frame buffer address that is not word aligned */
    DISPLAY_ERR_SIZE,       /* the picture is empty or larger than the limits */
    DISPLAY_ERR_TIMING,     /* a sync, back porch or front porch is zero or over the limit */
    DISPLAY_ERR_FORMAT,     /* not one of the two formats */
    DISPLAY_ERR_WINDOW,     /* the frame buffer does not fit the window it was given */
    DISPLAY_ERR_RECT        /* a rectangle of a DMA2D job falls outside the picture, or is empty */
} DisplayStatus;

static inline uint32_t Display_BytesPerPixel(uint8_t format)
{
    return format == DISPLAY_FMT_ARGB8888 ? 4u : 2u;
}

/* Works out the plan. `fb_addr` is the address of the frame buffer and `window_bytes` the size of the memory that belongs to it. */
static inline DisplayStatus Display_PlanMake(const DisplayPanel *panel, uint32_t fb_addr, uint32_t window_bytes, DisplayPlan *plan)
{
    if (!panel || !plan || (fb_addr & 3u) != 0u) return DISPLAY_ERR_ARGUMENT;
    if (panel->width == 0u || panel->height == 0u || panel->width > DISPLAY_MAX_WIDTH || panel->height > DISPLAY_MAX_HEIGHT) return DISPLAY_ERR_SIZE;
    const uint16_t t[6] = {panel->hsync, panel->hbp, panel->hfp, panel->vsync, panel->vbp, panel->vfp};
    for (unsigned i = 0; i < 6u; i++) {
        if (t[i] == 0u || t[i] > DISPLAY_MAX_SYNC) return DISPLAY_ERR_TIMING;
    }
    if (panel->format != DISPLAY_FMT_RGB565 && panel->format != DISPLAY_FMT_ARGB8888) return DISPLAY_ERR_FORMAT;

    const uint32_t bpp = Display_BytesPerPixel(panel->format);
    const uint32_t pitch = (uint32_t)panel->width * bpp;
    const uint32_t frame = pitch * panel->height;
    if (frame > window_bytes || (uint64_t)fb_addr + frame > 0x100000000ULL) return DISPLAY_ERR_WINDOW;

    const uint32_t h_acc = (uint32_t)panel->hsync + panel->hbp; /* the first active column + 1 */
    const uint32_t v_acc = (uint32_t)panel->vsync + panel->vbp;
    memset(plan, 0, sizeof *plan);
    plan->sscr = ((uint32_t)(panel->hsync - 1u) << 16) | (uint32_t)(panel->vsync - 1u);
    plan->bpcr = ((h_acc - 1u) << 16) | (v_acc - 1u);
    plan->awcr = ((h_acc + panel->width - 1u) << 16) | (v_acc + panel->height - 1u);
    plan->twcr = ((h_acc + panel->width + panel->hfp - 1u) << 16) | (v_acc + panel->height + panel->vfp - 1u);
    plan->whpcr = ((h_acc + panel->width - 1u) << 16) | h_acc;
    plan->wvpcr = ((v_acc + panel->height - 1u) << 16) | v_acc;
    plan->pfcr = panel->format == DISPLAY_FMT_ARGB8888 ? DISPLAY_LTDC_PF_ARGB8888 : DISPLAY_LTDC_PF_RGB565;
    plan->cfblr = (pitch << 16) | (pitch + 3u);
    plan->cfblnr = panel->height;
    plan->cfbar = fb_addr;
    plan->pitch_bytes = pitch;
    plan->frame_bytes = frame;
    plan->dma2d_cm = panel->format == DISPLAY_FMT_ARGB8888 ? DISPLAY_DMA2D_CM_ARGB8888 : DISPLAY_DMA2D_CM_RGB565;
    return DISPLAY_OK;
}

/* A rectangle of the picture, in pixels. */
typedef struct {
    uint16_t x, y, w, h;
} DisplayRect;

/* The registers of one DMA2D job: the mode, the output (and for a copy the foreground) address, the offsets between lines in pixels and the size PL:NL. */
typedef struct {
    uint32_t cr, opfccr, ocolr, omar, oor, nlr;
    uint32_t fgmar, fgor, fgpfccr;
} Dma2dJob;

static inline DisplayStatus Display_RectCheck(const DisplayPanel *panel, const DisplayRect *r)
{
    if (!panel || !r || r->w == 0u || r->h == 0u) return DISPLAY_ERR_RECT;
    if ((uint32_t)r->x + r->w > panel->width || (uint32_t)r->y + r->h > panel->height) return DISPLAY_ERR_RECT;
    return DISPLAY_OK;
}

static inline uint32_t Display_PixelAddr(const DisplayPlan *plan, const DisplayPanel *panel, uint16_t x, uint16_t y)
{
    return plan->cfbar + (uint32_t)y * plan->pitch_bytes + (uint32_t)x * Display_BytesPerPixel(panel->format);
}

/* A fill of `r` with `color` (in the format of the picture). */
static inline DisplayStatus Display_FillJob(const DisplayPlan *plan, const DisplayPanel *panel, const DisplayRect *r, uint32_t color, Dma2dJob *job)
{
    if (!plan || !job) return DISPLAY_ERR_ARGUMENT;
    const DisplayStatus s = Display_RectCheck(panel, r);
    if (s != DISPLAY_OK) return s;
    memset(job, 0, sizeof *job);
    job->cr = DISPLAY_DMA2D_CR_R2M;
    job->opfccr = plan->dma2d_cm;
    job->ocolr = color;
    job->omar = Display_PixelAddr(plan, panel, r->x, r->y);
    job->oor = (uint32_t)panel->width - r->w;
    job->nlr = ((uint32_t)r->w << 16) | r->h;
    return DISPLAY_OK;
}

/* A copy of a bitmap of `src_pitch_px` pixels per line, in the format of the picture, to `r` (the bitmap starts at the corner of the rectangle). */
static inline DisplayStatus Display_CopyJob(const DisplayPlan *plan, const DisplayPanel *panel, const DisplayRect *r, uint32_t src_addr, uint16_t src_pitch_px, Dma2dJob *job)
{
    if (!plan || !job || (src_addr & 3u) != 0u) return DISPLAY_ERR_ARGUMENT;
    const DisplayStatus s = Display_RectCheck(panel, r);
    if (s != DISPLAY_OK) return s;
    if (src_pitch_px < r->w) return DISPLAY_ERR_RECT;
    memset(job, 0, sizeof *job);
    job->cr = DISPLAY_DMA2D_CR_M2M;
    job->opfccr = plan->dma2d_cm;
    job->omar = Display_PixelAddr(plan, panel, r->x, r->y);
    job->oor = (uint32_t)panel->width - r->w;
    job->nlr = ((uint32_t)r->w << 16) | r->h;
    job->fgmar = src_addr;
    job->fgor = (uint32_t)src_pitch_px - r->w;
    job->fgpfccr = plan->dma2d_cm;
    return DISPLAY_OK;
}

#endif /* HYDRA_UMC_DISPLAY_PLAN_H */
