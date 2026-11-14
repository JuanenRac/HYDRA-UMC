/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_display_ltdc.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the numbers of the display (src/mcu_stm32h757/Common/display_plan.h) and of the LTDC and DMA2D driver (src/mcu_stm32h757/CM7/DisplayLtdc.c) against a double of the
 * registers (tests/stubs/h7ltdc). The plan is checked against the words worked out by hand for the starting panel of config/display.cfg and against the limits; the driver is checked
 * for the order of its writes (the layer is complete before the controller is enabled), for the jobs it gives the DMA2D and for a job that ends, fails, hangs or finds the block
 * busy. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_PLATFORM_H757 -DHYDRA_DISPLAY_LTDC=1 -Itests/stubs/h7ltdc -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/Common \
 *      -o test_display_ltdc tests/test_display_ltdc.c src/mcu_stm32h757/CM7/DisplayLtdc.c
 */
#include <stdio.h>
#include <string.h>

#include "stm32h7xx_hal.h"
#include "DisplayLtdc.h"

#if !HYDRA_DISPLAY_LTDC
#error "this test is for the driver with its switch on: build it with -DHYDRA_DISPLAY_LTDC=1"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

LTDC_TypeDef stub_ltdc;
LTDC_Layer_TypeDef stub_ltdc_layer1;
DMA2D_TypeDef stub_dma2d;

static int clock_mask;
void stub_clock_enable(int which) { clock_mask |= 1 << which; }

/* how the double plays the DMA2D: what it does at the next poll */
typedef enum { END_DONE, END_ERROR, END_NEVER } Ending;
static Ending ending;
static int relaxes;

static void relax(void)
{
    relaxes++;
    if (ending == END_DONE && relaxes >= 2) stub_dma2d.ISR = DMA2D_ISR_TCIF;
    if (ending == END_ERROR) stub_dma2d.ISR = DMA2D_ISR_TEIF;
}

static DisplayPanel start_panel(void)
{
    DisplayPanel p = {1280u, 720u, 4u, 60u, 60u, 4u, 16u, 16u, DISPLAY_FMT_RGB565};
    return p;
}

#define FB 0xC0000000UL
#define WINDOW 0x08000000UL

static void test_plan(void)
{
    DisplayPlan plan;
    DisplayPanel p = start_panel();
    CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_OK, "the starting panel is accepted");
    CHECK(plan.sscr == 0x00030003UL, "SSCR");
    CHECK(plan.bpcr == 0x003F0013UL, "BPCR");
    CHECK(plan.awcr == 0x053F02E3UL, "AWCR");
    CHECK(plan.twcr == 0x057B02F3UL, "TWCR");
    CHECK(plan.whpcr == 0x053F0040UL && plan.wvpcr == 0x02E30014UL, "the window of the layer");
    CHECK(plan.pfcr == DISPLAY_LTDC_PF_RGB565 && plan.dma2d_cm == DISPLAY_DMA2D_CM_RGB565, "RGB565 words");
    CHECK(plan.pitch_bytes == 2560u && plan.frame_bytes == 1843200u, "pitch and frame size of RGB565");
    CHECK(plan.cfblr == 0x0A000A03UL && plan.cfblnr == 720u && plan.cfbar == FB, "line length, lines and address");

    p.format = DISPLAY_FMT_ARGB8888;
    CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_OK, "ARGB8888 is accepted");
    CHECK(plan.pfcr == DISPLAY_LTDC_PF_ARGB8888 && plan.dma2d_cm == DISPLAY_DMA2D_CM_ARGB8888, "ARGB8888 words");
    CHECK(plan.pitch_bytes == 5120u && plan.frame_bytes == 3686400u, "pitch and frame size of ARGB8888");
    CHECK(Display_BytesPerPixel(DISPLAY_FMT_ARGB8888) == 4u && Display_BytesPerPixel(DISPLAY_FMT_RGB565) == 2u, "bytes per pixel");

    /* the refusals, one by one */
    p = start_panel();
    CHECK(Display_PlanMake(NULL, FB, WINDOW, &plan) == DISPLAY_ERR_ARGUMENT, "no panel");
    CHECK(Display_PlanMake(&p, FB, WINDOW, NULL) == DISPLAY_ERR_ARGUMENT, "no plan");
    CHECK(Display_PlanMake(&p, FB + 2u, WINDOW, &plan) == DISPLAY_ERR_ARGUMENT, "an address that is not word aligned");
    p.width = 0;
    CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_ERR_SIZE, "empty width");
    p = start_panel();
    p.height = 0;
    CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_ERR_SIZE, "empty height");
    p = start_panel();
    p.width = DISPLAY_MAX_WIDTH + 1u;
    CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_ERR_SIZE, "too wide");
    p = start_panel();
    p.height = DISPLAY_MAX_HEIGHT + 1u;
    CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_ERR_SIZE, "too high");
    for (unsigned i = 0; i < 6u; i++) {
        uint16_t *field[6];
        p = start_panel();
        field[0] = &p.hsync; field[1] = &p.hbp; field[2] = &p.hfp; field[3] = &p.vsync; field[4] = &p.vbp; field[5] = &p.vfp;
        *field[i] = 0u;
        CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_ERR_TIMING, "a zero timing");
        *field[i] = DISPLAY_MAX_SYNC + 1u;
        CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_ERR_TIMING, "a timing over the limit");
        *field[i] = DISPLAY_MAX_SYNC;
        CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_OK, "a timing at the limit");
    }
    p = start_panel();
    p.format = 7u;
    CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_ERR_FORMAT, "an unknown format");
    p = start_panel();
    CHECK(Display_PlanMake(&p, FB, 1843199UL, &plan) == DISPLAY_ERR_WINDOW, "a window one byte short");
    CHECK(Display_PlanMake(&p, FB, 1843200UL, &plan) == DISPLAY_OK, "a window of exactly the frame");
    CHECK(Display_PlanMake(&p, 0xFFF00000UL, WINDOW, &plan) == DISPLAY_ERR_WINDOW, "a frame buffer past the end of the address space");
}

static void test_jobs(void)
{
    DisplayPlan plan;
    DisplayPanel p = start_panel();
    Dma2dJob job;
    DisplayRect r = {10u, 20u, 100u, 50u};
    CHECK(Display_PlanMake(&p, FB, WINDOW, &plan) == DISPLAY_OK, "plan for the jobs");

    CHECK(Display_FillJob(&plan, &p, &r, 0xF800u, &job) == DISPLAY_OK, "a fill");
    CHECK(job.cr == DISPLAY_DMA2D_CR_R2M && job.opfccr == 2u && job.ocolr == 0xF800u, "mode, format and colour of the fill");
    CHECK(job.omar == FB + 20UL * 2560UL + 20UL, "the first pixel of the fill");
    CHECK(job.oor == 1180u && job.nlr == ((100UL << 16) | 50UL), "line offset and size of the fill");

    CHECK(Display_CopyJob(&plan, &p, &r, 0xC1000000UL, 128u, &job) == DISPLAY_OK, "a copy");
    CHECK(job.cr == DISPLAY_DMA2D_CR_M2M && job.fgmar == 0xC1000000UL && job.fgor == 28u && job.fgpfccr == 2u, "source of the copy");
    CHECK(job.omar == FB + 20UL * 2560UL + 20UL && job.oor == 1180u, "destination of the copy");

    /* the refusals */
    CHECK(Display_FillJob(NULL, &p, &r, 0u, &job) == DISPLAY_ERR_ARGUMENT, "fill: no plan");
    CHECK(Display_FillJob(&plan, &p, &r, 0u, NULL) == DISPLAY_ERR_ARGUMENT, "fill: no job");
    CHECK(Display_CopyJob(NULL, &p, &r, 0xC1000000UL, 128u, &job) == DISPLAY_ERR_ARGUMENT, "copy: no plan");
    CHECK(Display_CopyJob(&plan, &p, &r, 0xC1000000UL, 128u, NULL) == DISPLAY_ERR_ARGUMENT, "copy: no job");
    CHECK(Display_CopyJob(&plan, &p, &r, 0xC1000002UL, 128u, &job) == DISPLAY_ERR_ARGUMENT, "copy: source not word aligned");
    CHECK(Display_CopyJob(&plan, &p, &r, 0xC1000000UL, 99u, &job) == DISPLAY_ERR_RECT, "copy: line of the source shorter than the rectangle");
    CHECK(Display_CopyJob(&plan, &p, &r, 0xC1000000UL, 100u, &job) == DISPLAY_OK, "copy: a source line exactly as long");
    DisplayRect bad = {0u, 0u, 0u, 5u};
    CHECK(Display_RectCheck(&p, &bad) == DISPLAY_ERR_RECT, "an empty width");
    bad.w = 5u; bad.h = 0u;
    CHECK(Display_RectCheck(&p, &bad) == DISPLAY_ERR_RECT, "an empty height");
    bad.h = 5u; bad.x = 1276u;
    CHECK(Display_RectCheck(&p, &bad) == DISPLAY_ERR_RECT, "past the right edge");
    bad.x = 1275u; bad.y = 716u;
    CHECK(Display_RectCheck(&p, &bad) == DISPLAY_ERR_RECT, "past the bottom edge");
    bad.y = 715u;
    CHECK(Display_RectCheck(&p, &bad) == DISPLAY_OK, "exactly the corner");
    CHECK(Display_RectCheck(NULL, &bad) == DISPLAY_ERR_RECT, "no panel");
    CHECK(Display_RectCheck(&p, NULL) == DISPLAY_ERR_RECT, "no rectangle");
}

static DisplayLtdcConfig good(void)
{
    DisplayLtdcConfig c;
    memset(&c, 0, sizeof c);
    c.panel = start_panel();
    c.fb_addr = FB;
    c.fb_window_bytes = WINDOW;
    c.poll_limit = 50u;
    c.relax = relax;
    return c;
}

static void test_driver(void)
{
    DisplayRect r = {0u, 0u, 16u, 16u};
    DisplayLtdcConfig c = good();

    /* before a successful start nothing moves */
    CHECK(DisplayLtdc_Fill(&r, 1u) == DISPLAYLTDC_ERR_NOT_STARTED, "a fill before the start");
    CHECK(DisplayLtdc_Copy(&r, FB, 16u) == DISPLAYLTDC_ERR_NOT_STARTED, "a copy before the start");
    CHECK(DisplayLtdc_Plan() == NULL, "no plan before the start");
    CHECK(DisplayLtdc_Init(NULL) == 0, "no configuration");
    c.poll_limit = 0u;
    CHECK(DisplayLtdc_Init(&c) == 0, "poll limit 0");
    c.poll_limit = 1000001u;
    CHECK(DisplayLtdc_Init(&c) == 0, "poll limit over the maximum");
    c = good();
    c.panel.hsync = 0u;
    CHECK(DisplayLtdc_Init(&c) == 0, "a bad panel");
    CHECK(clock_mask == 0 && stub_ltdc.GCR == 0u && stub_ltdc_layer1.CR == 0u, "a refused start touched nothing");

    /* a good start; the controller was on, as after a warm restart */
    c = good();
    c.relax = NULL;
    stub_ltdc.GCR = LTDC_GCR_LTDCEN;
    CHECK(DisplayLtdc_Init(&c) == 1, "a good start");
    CHECK(clock_mask == 6, "both clocks are enabled");
    CHECK(stub_ltdc.SSCR == 0x00030003UL && stub_ltdc.BPCR == 0x003F0013UL && stub_ltdc.AWCR == 0x053F02E3UL && stub_ltdc.TWCR == 0x057B02F3UL, "the timing words");
    CHECK(stub_ltdc_layer1.WHPCR == 0x053F0040UL && stub_ltdc_layer1.WVPCR == 0x02E30014UL, "the window words");
    CHECK(stub_ltdc_layer1.PFCR == 2u && stub_ltdc_layer1.CACR == 255u && stub_ltdc_layer1.DCCR == 0u && stub_ltdc_layer1.BFCR == 0x0405UL, "format, alpha and blending");
    CHECK(stub_ltdc_layer1.CFBAR == FB && stub_ltdc_layer1.CFBLR == 0x0A000A03UL && stub_ltdc_layer1.CFBLNR == 720u, "frame buffer words");
    CHECK((stub_ltdc_layer1.CR & LTDC_LxCR_LEN) != 0u, "the layer is on");
    CHECK(stub_ltdc.SRCR == LTDC_SRCR_IMR, "the shadow reload was asked for");
    CHECK((stub_ltdc.GCR & LTDC_GCR_LTDCEN) != 0u, "the controller is on");
    CHECK(DisplayLtdc_Plan() != NULL && DisplayLtdc_Plan()->frame_bytes == 1843200u, "the plan in use");

    /* a fill that ends after two polls */
    c = good();
    CHECK(DisplayLtdc_Init(&c) == 1, "start again with the relax hook");
    ending = END_DONE;
    relaxes = 0;
    stub_dma2d.ISR = 0u;
    stub_dma2d.CR = 0u;
    CHECK(DisplayLtdc_Fill(&r, 0x07E0u) == DISPLAYLTDC_OK, "a fill ends");
    CHECK(relaxes == 2, "it polled until the flag came");
    CHECK(stub_dma2d.OCOLR == 0x07E0u && stub_dma2d.OMAR == FB && stub_dma2d.NLR == ((16UL << 16) | 16UL) && stub_dma2d.OOR == 1264u && stub_dma2d.OPFCCR == 2u, "the registers of the fill");
    CHECK(stub_dma2d.CR == (DISPLAY_DMA2D_CR_R2M | DISPLAY_DMA2D_CR_START), "the job was started in register-to-memory mode");

    /* a copy */
    relaxes = 0;
    stub_dma2d.ISR = 0u;
    stub_dma2d.CR = 0u;
    CHECK(DisplayLtdc_Copy(&r, 0xC1000000UL, 32u) == DISPLAYLTDC_OK, "a copy ends");
    CHECK(stub_dma2d.FGMAR == 0xC1000000UL && stub_dma2d.FGOR == 16u && stub_dma2d.FGPFCCR == 2u && stub_dma2d.CR == (DISPLAY_DMA2D_CR_M2M | DISPLAY_DMA2D_CR_START), "the registers of the copy");

    /* refused requests never reach the registers */
    stub_dma2d.NLR = 0u;
    DisplayRect off = {1270u, 0u, 16u, 16u};
    CHECK(DisplayLtdc_Fill(&off, 1u) == DISPLAYLTDC_ERR_BAD_REQUEST, "a fill outside the picture");
    CHECK(DisplayLtdc_Copy(&off, FB, 32u) == DISPLAYLTDC_ERR_BAD_REQUEST, "a copy outside the picture");
    CHECK(stub_dma2d.NLR == 0u, "the registers were not written");

    /* a block that is still busy */
    stub_dma2d.CR = DISPLAY_DMA2D_CR_START;
    CHECK(DisplayLtdc_Fill(&r, 1u) == DISPLAYLTDC_ERR_BUSY, "the previous job is still running");
    stub_dma2d.CR = 0u;

    /* a job that fails */
    ending = END_ERROR;
    stub_dma2d.ISR = 0u;
    CHECK(DisplayLtdc_Fill(&r, 1u) == DISPLAYLTDC_ERR_TRANSFER, "a transfer error");
    stub_dma2d.CR = 0u;
    stub_dma2d.ISR = DMA2D_ISR_CEIF; /* a configuration error is the same answer */
    ending = END_NEVER;
    CHECK(DisplayLtdc_Fill(&r, 1u) == DISPLAYLTDC_ERR_TRANSFER, "a configuration error");

    /* a job that never ends */
    stub_dma2d.ISR = 0u;
    stub_dma2d.CR = 0u;
    relaxes = 0;
    CHECK(DisplayLtdc_Fill(&r, 1u) == DISPLAYLTDC_ERR_TIMEOUT, "a job that hangs is given up");
    CHECK(relaxes == 50, "it polled exactly the limit");
    CHECK(stub_dma2d.CR == 0u, "the block was stopped");

    /* without a relax hook a hanging job still ends */
    c.relax = NULL;
    CHECK(DisplayLtdc_Init(&c) == 1, "start without a hook");
    stub_dma2d.ISR = 0u;
    CHECK(DisplayLtdc_Fill(&r, 1u) == DISPLAYLTDC_ERR_TIMEOUT, "it times out without a hook too");
}

int main(void)
{
    test_plan();
    test_jobs();
    test_driver();
    if (failures) {
        printf("display_ltdc host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("display_ltdc host logic tests passed\n");
    return 0;
}
