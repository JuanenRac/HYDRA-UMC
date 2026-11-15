/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_ui_flush_plan.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the flush plan of the interface library (src/mcu_stm32h757/Common/ui_flush_plan.h): the lines that fit the render buffer in both formats (the same numbers as
 * tests/test_ui_flush_plan.py against the Python twin), the strips and the short last one, the rectangle of an area in the inclusive coordinates of the library, the copy job of a
 * flush (the address of the first pixel, the offsets of both sides, the size), and the refusal of every bad size, index and area. A sweep checks that the strips tile the picture
 * exactly, once, without a gap. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/mcu_stm32h757/Common -o test_ui_flush_plan tests/test_ui_flush_plan.c
 */
#include <stdio.h>
#include <string.h>

#include "ui_flush_plan.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static DisplayPanel panel(uint16_t w, uint16_t h, uint8_t format)
{
    DisplayPanel p = {w, h, 4u, 60u, 60u, 4u, 16u, 16u, format};
    return p;
}

int main(void)
{
    DisplayPanel p = panel(1280u, 720u, DISPLAY_FMT_RGB565);
    UfpConfig c = {180u, 1u};
    UfpPlan plan;

    /* the plan */
    CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_OK && plan.lines == 72u && plan.buffer_bytes == 72u * 2560u && plan.strips == 10u && plan.total_bytes == plan.buffer_bytes && plan.buffers == 1u, "180 KiB (a tenth of the frame) hold 72 lines of 2560 bytes: 10 strips");
    c.buffer_kb = 2048u;
    CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_OK && plan.lines == 720u && plan.strips == 1u, "a buffer larger than the picture holds all the lines: one strip");
    c.buffer_kb = 4u; c.buffers = 2u;
    CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_OK && plan.lines == 1u && plan.strips == 720u && plan.total_bytes == 2u * 2560u, "4 KiB hold one line; two buffers");
    p = panel(1280u, 720u, DISPLAY_FMT_ARGB8888); c.buffer_kb = 180u; c.buffers = 1u;
    CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_OK && plan.lines == 36u && plan.strips == 20u, "in 32 bits: 36 lines, 20 strips");
    p = panel(2048u, 2048u, DISPLAY_FMT_ARGB8888); c.buffer_kb = 4u;
    CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_E_SMALL, "4 KiB do not hold a line of 8 KiB");

    /* refusals of the plan */
    p = panel(1280u, 720u, DISPLAY_FMT_RGB565); c.buffer_kb = 180u; c.buffers = 1u;
    CHECK(UfpPlan_Make(NULL, &c, &plan) == UFP_E_ARG && UfpPlan_Make(&p, NULL, &plan) == UFP_E_ARG && UfpPlan_Make(&p, &c, NULL) == UFP_E_ARG, "missing arguments");
    c.buffer_kb = 3u;    CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_E_BUFFER, "under 4 KiB");
    c.buffer_kb = 2049u; CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_E_BUFFER, "over 2048 KiB");
    c.buffer_kb = 180u; c.buffers = 0u; CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_E_BUFFER, "no buffer");
    c.buffers = 3u;      CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_E_BUFFER, "three buffers");
    c.buffers = 1u;
    p = panel(0u, 720u, DISPLAY_FMT_RGB565);   CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_E_PANEL, "no width");
    p = panel(1280u, 0u, DISPLAY_FMT_RGB565);  CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_E_PANEL, "no height");
    p = panel(1280u, 720u, 7u);                CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_E_PANEL, "a format that does not exist");

    /* the strips */
    p = panel(1280u, 720u, DISPLAY_FMT_RGB565);
    CHECK(UfpPlan_Make(&p, &c, &plan) == UFP_OK, "the plan for the strips");
    DisplayRect r;
    CHECK(UfpPlan_Strip(&p, &plan, 0u, &r) == UFP_OK && r.x == 0u && r.y == 0u && r.w == 1280u && r.h == 72u, "the first strip");
    CHECK(UfpPlan_Strip(&p, &plan, 9u, &r) == UFP_OK && r.y == 648u && r.h == 72u, "with 720 lines in tenths the last strip is as long as the others");
    CHECK(UfpPlan_Strip(&p, &plan, 10u, &r) == UFP_E_INDEX, "there is no strip 10");
    CHECK(UfpPlan_Strip(NULL, &plan, 0u, &r) == UFP_E_ARG && UfpPlan_Strip(&p, NULL, 0u, &r) == UFP_E_ARG && UfpPlan_Strip(&p, &plan, 0u, NULL) == UFP_E_ARG, "missing arguments");

    /* the tiling sweep */
    unsigned tiled = 0u;
    static const uint16_t heights[] = {1u, 7u, 100u, 480u, 719u, 720u, 1000u};
    static const uint32_t sizes[] = {4u, 13u, 64u, 180u, 1024u};
    for (unsigned a = 0u; a < sizeof heights / sizeof heights[0]; a++) {
        for (unsigned b = 0u; b < sizeof sizes / sizeof sizes[0]; b++) {
            const DisplayPanel q = panel(320u, heights[a], DISPLAY_FMT_RGB565);
            UfpConfig k = {sizes[b], 1u};
            UfpPlan pl;
            if (UfpPlan_Make(&q, &k, &pl) != UFP_OK) continue;
            uint32_t next = 0u;
            int ok = 1;
            for (uint32_t i = 0u; i < pl.strips; i++) {
                DisplayRect s;
                ok &= UfpPlan_Strip(&q, &pl, i, &s) == UFP_OK && s.y == next && s.h >= 1u && s.h <= pl.lines && s.w == q.width;
                next += s.h;
            }
            CHECK(ok && next == q.height, "the strips tile the picture exactly once");
            tiled++;
        }
    }
    CHECK(tiled >= 30u, "the sweep covered the combinations");

    /* an area of the library */
    UfpArea area = {10, 20, 109, 59};
    CHECK(UfpPlan_Rect(&p, &area, &r) == UFP_OK && r.x == 10u && r.y == 20u && r.w == 100u && r.h == 40u, "inclusive coordinates: 100 x 40");
    area.x1 = 0; area.y1 = 0; area.x2 = 1279; area.y2 = 719;
    CHECK(UfpPlan_Rect(&p, &area, &r) == UFP_OK && r.w == 1280u && r.h == 720u, "the whole picture");
    area.x1 = 5; area.y1 = 5; area.x2 = 5; area.y2 = 5;
    CHECK(UfpPlan_Rect(&p, &area, &r) == UFP_OK && r.w == 1u && r.h == 1u, "one pixel");
    UfpArea bad[] = {{-1, 0, 5, 5}, {0, -1, 5, 5}, {6, 0, 5, 5}, {0, 6, 5, 5}, {0, 0, 1280, 5}, {0, 0, 5, 720}};
    for (unsigned i = 0u; i < sizeof bad / sizeof bad[0]; i++) CHECK(UfpPlan_Rect(&p, &bad[i], &r) == UFP_E_AREA, "an area that is negative, inverted or outside");
    CHECK(UfpPlan_Rect(NULL, &area, &r) == UFP_E_ARG && UfpPlan_Rect(&p, NULL, &r) == UFP_E_ARG && UfpPlan_Rect(&p, &area, NULL) == UFP_E_ARG, "missing arguments");

    /* the flush job */
    DisplayPlan dp;
    CHECK(Display_PlanMake(&p, 0xC0000000UL, 0x400000UL, &dp) == DISPLAY_OK, "the display plan");
    Dma2dJob job;
    area.x1 = 10; area.y1 = 20; area.x2 = 109; area.y2 = 59;
    CHECK(UfpPlan_FlushJob(&dp, &p, &area, 0x24000000UL, &job) == UFP_OK, "a flush");
    CHECK(job.cr == DISPLAY_DMA2D_CR_M2M && job.omar == 0xC0000000UL + 20u * 2560u + 10u * 2u && job.oor == 1280u - 100u && job.fgmar == 0x24000000UL && job.fgor == 0u && job.nlr == ((100u << 16) | 40u), "the first pixel in the frame buffer, the offsets of both sides, the size");
    CHECK(UfpPlan_FlushJob(&dp, &p, &area, 0x24000002UL, &job) == UFP_E_JOB, "a source address that is not word aligned");
    CHECK(UfpPlan_FlushJob(&dp, &p, &bad[0], 0x24000000UL, &job) == UFP_E_AREA, "a bad area is refused first");
    CHECK(UfpPlan_FlushJob(NULL, &p, &area, 0x24000000UL, &job) == UFP_E_JOB, "no display plan");

    if (failures) {
        printf("ui_flush_plan host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("ui_flush_plan host logic tests passed\n");
    return 0;
}
