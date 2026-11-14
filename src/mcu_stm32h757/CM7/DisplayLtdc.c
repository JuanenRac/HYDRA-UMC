/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/DisplayLtdc.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Driver of the LTDC and the DMA2D; see DisplayLtdc.h and Common/display_plan.h. The registers are written in the order the reference manual asks: timing, layer, shadow reload
 * and only then the enable bits, so that nothing is shown before the layer is complete. A DMA2D job is polled up to the limit of the configuration and a job that reports an error
 * leaves the flags cleared for the next one.
 * ========================================================================== */
#include "DisplayLtdc.h"

#if HYDRA_DISPLAY_LTDC

#include "stm32h7xx_hal.h"

#ifndef HYDRA_PLATFORM_H757
#error "DisplayLtdc is the driver of the H757 board: compile it with HYDRA_PLATFORM_H757"
#endif

#define DISPLAY_POLL_LIMIT_MAX 1000000u

/* not named by the vendor header: the blending factors of the layer (constant alpha) and the clear bits of every DMA2D flag */
#ifndef LTDC_BFCR_CONSTANT_ALPHA
#define LTDC_BFCR_CONSTANT_ALPHA 0x00000405UL
#endif
#ifndef DMA2D_IFCR_ALL
#define DMA2D_IFCR_ALL 0x0000003FUL
#endif

static DisplayLtdcConfig g_cfg;
static DisplayPlan g_plan;
static int g_ready;

static void DisplayLtdc_Relax(void)
{
    if (g_cfg.relax != NULL) g_cfg.relax();
}

int DisplayLtdc_Init(const DisplayLtdcConfig *cfg)
{
    g_ready = 0;
    if (cfg == NULL) return 0;
    if (cfg->poll_limit < 1u || cfg->poll_limit > DISPLAY_POLL_LIMIT_MAX) return 0;
    DisplayPlan plan;
    if (Display_PlanMake(&cfg->panel, cfg->fb_addr, cfg->fb_window_bytes, &plan) != DISPLAY_OK) return 0;
    g_cfg = *cfg;
    g_plan = plan;

    __HAL_RCC_LTDC_CLK_ENABLE();
    __HAL_RCC_DMA2D_CLK_ENABLE();

    LTDC->GCR &= ~LTDC_GCR_LTDCEN; /* the timing is written with the controller off */
    LTDC->SSCR = plan.sscr;
    LTDC->BPCR = plan.bpcr;
    LTDC->AWCR = plan.awcr;
    LTDC->TWCR = plan.twcr;

    LTDC_Layer1->WHPCR = plan.whpcr;
    LTDC_Layer1->WVPCR = plan.wvpcr;
    LTDC_Layer1->PFCR = plan.pfcr;
    LTDC_Layer1->CACR = 255u; /* opaque */
    LTDC_Layer1->DCCR = 0u;
    LTDC_Layer1->BFCR = LTDC_BFCR_CONSTANT_ALPHA;
    LTDC_Layer1->CFBAR = plan.cfbar;
    LTDC_Layer1->CFBLR = plan.cfblr;
    LTDC_Layer1->CFBLNR = plan.cfblnr;
    LTDC_Layer1->CR |= LTDC_LxCR_LEN;
    LTDC->SRCR = LTDC_SRCR_IMR; /* the layer registers take effect */
    LTDC->GCR |= LTDC_GCR_LTDCEN;

    g_ready = 1;
    return 1;
}

static DisplayLtdcStatus DisplayLtdc_Run(const Dma2dJob *job)
{
    if (DMA2D->CR & DISPLAY_DMA2D_CR_START) return DISPLAYLTDC_ERR_BUSY;
    DMA2D->IFCR = DMA2D_IFCR_ALL;
    DMA2D->OPFCCR = job->opfccr;
    DMA2D->OCOLR = job->ocolr;
    DMA2D->OMAR = job->omar;
    DMA2D->OOR = job->oor;
    DMA2D->NLR = job->nlr;
    DMA2D->FGMAR = job->fgmar;
    DMA2D->FGOR = job->fgor;
    DMA2D->FGPFCCR = job->fgpfccr;
    DMA2D->CR = job->cr | DISPLAY_DMA2D_CR_START;
    for (uint32_t polls = 0; polls < g_cfg.poll_limit; polls++) {
        const uint32_t isr = DMA2D->ISR;
        if (isr & (DMA2D_ISR_TEIF | DMA2D_ISR_CEIF)) {
            DMA2D->IFCR = DMA2D_IFCR_ALL;
            return DISPLAYLTDC_ERR_TRANSFER;
        }
        if (isr & DMA2D_ISR_TCIF) {
            DMA2D->IFCR = DMA2D_IFCR_ALL;
            return DISPLAYLTDC_OK;
        }
        DisplayLtdc_Relax();
    }
    DMA2D->CR = 0u; /* give the job up: the next one starts from a stopped block */
    DMA2D->IFCR = DMA2D_IFCR_ALL;
    return DISPLAYLTDC_ERR_TIMEOUT;
}

DisplayLtdcStatus DisplayLtdc_Fill(const DisplayRect *rect, uint32_t color)
{
    if (!g_ready) return DISPLAYLTDC_ERR_NOT_STARTED;
    Dma2dJob job;
    if (Display_FillJob(&g_plan, &g_cfg.panel, rect, color, &job) != DISPLAY_OK) return DISPLAYLTDC_ERR_BAD_REQUEST;
    return DisplayLtdc_Run(&job);
}

DisplayLtdcStatus DisplayLtdc_Copy(const DisplayRect *rect, uint32_t src_addr, uint16_t src_pitch_px)
{
    if (!g_ready) return DISPLAYLTDC_ERR_NOT_STARTED;
    Dma2dJob job;
    if (Display_CopyJob(&g_plan, &g_cfg.panel, rect, src_addr, src_pitch_px, &job) != DISPLAY_OK) return DISPLAYLTDC_ERR_BAD_REQUEST;
    return DisplayLtdc_Run(&job);
}

const DisplayPlan *DisplayLtdc_Plan(void)
{
    return g_ready ? &g_plan : NULL;
}

#endif /* HYDRA_DISPLAY_LTDC */
