/* =============================================================================
 * HYDRA-UMC Firmware - config/lv_conf.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The settings of the interface library of the H757 board (the library is fetched by its commit, see config/lvgl.cfg and tools/lvgl_pin.py; this file is OURS, the library only reads it).
 * Starting values for the bench. The build in Actions compiles the library with them and checks the flash it takes against the budget. Everything not set here keeps the default of
 * the library. Evidence: simulator; nothing here has run on a board.
 * ============================================================================= */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 16 bits per pixel: the frame buffer of the LTDC is RGB565 (config/display.cfg) */
#define LV_COLOR_DEPTH 16

/* the heap is a fixed pool of the library inside the AXI SRAM, so that its exhaustion degrades the drawing and never the stop path (docs/UI_STACK_H757.md) */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (96U * 1024U)

/* no operating system calls inside the library: the interface task owns every call into it */
#define LV_USE_OS LV_OS_NONE

/* no floating point, no logging, no examples, no demos: they are not part of the budget */
#define LV_USE_FLOAT 0
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_DEMO_WIDGETS 0
#define LV_BUILD_EXAMPLES 0

/* the copy to the frame buffer is the DMA2D job of Common/ui_flush_plan.h, not a drawing unit of the library */
#define LV_USE_DRAW_DMA2D 0

/* the picture is drawn in strips of whole lines (config/ui_flush.cfg) */
#define LV_DEF_REFR_PERIOD 33

#endif /* LV_CONF_H */
