// =============================================================================
// HYDRA-UMC Kinematic Brain - start-up of the external SDRAM of the STM32H757 board (FMC), header-only and free of the HAL
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The C twin of tools/sdram_plan.py: from the numbers of the platform profile (geometry, CAS latency, the timings in SDRAM clock cycles, the refresh period, which banks
// are enabled and how large a window each one has) it computes the values of the FMC registers SDCR, SDTR, SDCMR and SDRTR and runs the JEDEC start-up sequence: clock
// enable, a wait of at least 100 us, precharge all, auto-refresh cycles, load of the mode register and the refresh rate. The registers are reached only through the
// callbacks of SdramOps, so the host test runs the sequence against a double and compares every write with the golden plan of the Python model.
//
// The SDRAM may be used only after Sdram_Start has returned SDRAM_OK AND after the memory tests the board guide asks for (data, address, marching, temperature); this
// header does not run those. Starting values come from the CubeMX project and are still to be confirmed with the memory datasheets. Only for the H757 platform.
// Nothing here has run on a board.
// =============================================================================
#ifndef HYDRA_SDRAM_INIT_H
#define HYDRA_SDRAM_INIT_H

#include <stdint.h>
#include "platform_profile.h"

#if defined(HYDRA_PLATFORM_H757)

// the settings the geometry and the banks must satisfy, checked when the file compiles
#define HYDRA_SDRAM_CAPACITY_BYTES ((1UL << (HYDRA_SDRAM_ROW_BITS + HYDRA_SDRAM_COLUMN_BITS)) * HYDRA_SDRAM_INTERNAL_BANKS * (HYDRA_SDRAM_BUS_WIDTH_BITS / 8U))
_Static_assert(HYDRA_SDRAM_ROW_BITS >= 11U && HYDRA_SDRAM_ROW_BITS <= 13U, "rows must be 11 to 13 bits");
_Static_assert(HYDRA_SDRAM_COLUMN_BITS >= 8U && HYDRA_SDRAM_COLUMN_BITS <= 11U, "columns must be 8 to 11 bits");
_Static_assert(HYDRA_SDRAM_BUS_WIDTH_BITS == 8U || HYDRA_SDRAM_BUS_WIDTH_BITS == 16U || HYDRA_SDRAM_BUS_WIDTH_BITS == 32U, "bus width 8, 16 or 32 bits");
_Static_assert(HYDRA_SDRAM_INTERNAL_BANKS == 2U || HYDRA_SDRAM_INTERNAL_BANKS == 4U, "2 or 4 internal banks");
_Static_assert(HYDRA_SDRAM_CAS_LATENCY >= 1U && HYDRA_SDRAM_CAS_LATENCY <= 3U, "CAS latency 1 to 3");
_Static_assert(HYDRA_SDRAM_SDCLK_PERIOD_HCLK == 2U || HYDRA_SDRAM_SDCLK_PERIOD_HCLK == 3U, "the SDRAM clock is 2 or 3 times the period of HCLK");
_Static_assert(HYDRA_SDRAM_TRAS <= HYDRA_SDRAM_TRC, "tRAS cannot be longer than tRC");
_Static_assert(HYDRA_SDRAM_STARTUP_DELAY_US >= 100U, "the JEDEC start-up wait is at least 100 us");
_Static_assert(HYDRA_SDRAM_AUTO_REFRESH_CYCLES >= 2U && HYDRA_SDRAM_AUTO_REFRESH_CYCLES <= 16U, "2 to 16 auto-refresh cycles");
_Static_assert(!HYDRA_SDRAM_BANK1_ENABLED || HYDRA_SDRAM_BANK1_SIZE_BYTES <= HYDRA_SDRAM_CAPACITY_BYTES, "bank 1 window larger than the geometry holds");
_Static_assert(!HYDRA_SDRAM_BANK2_ENABLED || HYDRA_SDRAM_BANK2_SIZE_BYTES <= HYDRA_SDRAM_CAPACITY_BYTES, "bank 2 window larger than the geometry holds");
_Static_assert(HYDRA_SDRAM_BANK1_ENABLED || HYDRA_SDRAM_BANK2_ENABLED, "no SDRAM bank is enabled");

#define SDRAM_MODE_CLOCK_ENABLE 1UL
#define SDRAM_MODE_PALL 2UL
#define SDRAM_MODE_AUTO_REFRESH 3UL
#define SDRAM_MODE_LOAD_MODE 4UL
#define SDRAM_CTB2 (1UL << 3)
#define SDRAM_CTB1 (1UL << 4)
#define SDRAM_MODE_REGISTER_WRITE_SINGLE 0x0200UL
#define SDRAM_SDCR2_MASK 0x3FFUL                                    /* bank 2 takes the clock period, the burst, tRC and tRP from bank 1 */
#define SDRAM_SDTR2_MASK (0xFFFFFFFFUL & ~((0xFUL << 12) | (0xFUL << 20)))

#define SDRAM_MWID ((HYDRA_SDRAM_BUS_WIDTH_BITS == 8U) ? 0UL : (HYDRA_SDRAM_BUS_WIDTH_BITS == 16U) ? 1UL : 2UL)
#define SDRAM_SDCR_VALUE ((unsigned long)(HYDRA_SDRAM_COLUMN_BITS - 8U) | ((unsigned long)(HYDRA_SDRAM_ROW_BITS - 11U) << 2) | (SDRAM_MWID << 4) | \
                          ((HYDRA_SDRAM_INTERNAL_BANKS == 4U ? 1UL : 0UL) << 6) | ((unsigned long)HYDRA_SDRAM_CAS_LATENCY << 7) | \
                          ((unsigned long)HYDRA_SDRAM_SDCLK_PERIOD_HCLK << 10) | ((unsigned long)HYDRA_SDRAM_READ_BURST << 12) | \
                          ((unsigned long)HYDRA_SDRAM_READ_PIPE_DELAY_HCLK << 13))
#define SDRAM_SDTR_VALUE (((unsigned long)(HYDRA_SDRAM_TMRD - 1U)) | ((unsigned long)(HYDRA_SDRAM_TXSR - 1U) << 4) | ((unsigned long)(HYDRA_SDRAM_TRAS - 1U) << 8) | \
                          ((unsigned long)(HYDRA_SDRAM_TRC - 1U) << 12) | ((unsigned long)(HYDRA_SDRAM_TWR - 1U) << 16) | ((unsigned long)(HYDRA_SDRAM_TRP - 1U) << 20) | \
                          ((unsigned long)(HYDRA_SDRAM_TRCD - 1U) << 24))
#define SDRAM_MODE_REGISTER_VALUE (((unsigned long)HYDRA_SDRAM_CAS_LATENCY << 4) | SDRAM_MODE_REGISTER_WRITE_SINGLE)
// the refresh period of a row in SDRAM clock cycles, less the margin: period / rows * clock, in whole numbers
#define SDRAM_SDCLK_HZ ((unsigned long long)HYDRA_CLOCK_HCLK_HZ / HYDRA_SDRAM_SDCLK_PERIOD_HCLK)
#define SDRAM_REFRESH_COUNT ((unsigned long)((unsigned long long)HYDRA_SDRAM_REFRESH_PERIOD_MS * SDRAM_SDCLK_HZ / (1000ULL * (1ULL << HYDRA_SDRAM_ROW_BITS))) - HYDRA_SDRAM_REFRESH_MARGIN_CYCLES)
_Static_assert(SDRAM_REFRESH_COUNT >= 1UL && SDRAM_REFRESH_COUNT <= 0x1FFFUL, "the refresh count must fit the 13 bits of SDRTR");

#define SDRAM_TARGETS ((HYDRA_SDRAM_BANK1_ENABLED ? SDRAM_CTB1 : 0UL) | (HYDRA_SDRAM_BANK2_ENABLED ? SDRAM_CTB2 : 0UL))
#define SDRAM_SDCMR(mode, rounds, mrd) ((unsigned long)(mode) | SDRAM_TARGETS | ((unsigned long)((rounds) - 1U) << 5) | ((unsigned long)(mrd) << 9))

typedef enum { SDRAM_REG_SDCR1 = 0, SDRAM_REG_SDTR1, SDRAM_REG_SDCR2, SDRAM_REG_SDTR2, SDRAM_REG_SDCMR, SDRAM_REG_SDRTR } SdramReg;
typedef enum { SDRAM_OK = 0, SDRAM_ERR_BUSY_TIMEOUT = 1 } SdramStatus;

typedef struct {
    void (*write_reg)(void *ctx, SdramReg reg, uint32_t value);
    int (*wait_not_busy)(void *ctx);   /* waits for the BUSY flag of SDSR to fall; 1 when it did, 0 on a time-out */
    void (*wait_us)(void *ctx, uint32_t microseconds);
} SdramOps;

// the SDCMR command goes out only when the controller is not busy with the one before
static inline SdramStatus Sdram_Command(const SdramOps *ops, void *ctx, uint32_t value)
{
    if (!ops->wait_not_busy(ctx)) return SDRAM_ERR_BUSY_TIMEOUT;
    ops->write_reg(ctx, SDRAM_REG_SDCMR, value);
    return SDRAM_OK;
}

// The whole start-up. The registers of both banks are written (bank 2 takes the clock period, the burst, tRC and tRP from bank 1), and the commands go to the banks that
// are enabled: the target bits of SDCMR follow HYDRA_SDRAM_BANK1_ENABLED and HYDRA_SDRAM_BANK2_ENABLED.
static inline SdramStatus Sdram_Start(const SdramOps *ops, void *ctx)
{
    ops->write_reg(ctx, SDRAM_REG_SDCR1, (uint32_t)SDRAM_SDCR_VALUE);
    ops->write_reg(ctx, SDRAM_REG_SDTR1, (uint32_t)SDRAM_SDTR_VALUE);
    ops->write_reg(ctx, SDRAM_REG_SDCR2, (uint32_t)(SDRAM_SDCR_VALUE & SDRAM_SDCR2_MASK));
    ops->write_reg(ctx, SDRAM_REG_SDTR2, (uint32_t)(SDRAM_SDTR_VALUE & SDRAM_SDTR2_MASK));
    SdramStatus status;
    if ((status = Sdram_Command(ops, ctx, (uint32_t)SDRAM_SDCMR(SDRAM_MODE_CLOCK_ENABLE, 1U, 0U))) != SDRAM_OK) return status;
    ops->wait_us(ctx, HYDRA_SDRAM_STARTUP_DELAY_US);
    if ((status = Sdram_Command(ops, ctx, (uint32_t)SDRAM_SDCMR(SDRAM_MODE_PALL, 1U, 0U))) != SDRAM_OK) return status;
    if ((status = Sdram_Command(ops, ctx, (uint32_t)SDRAM_SDCMR(SDRAM_MODE_AUTO_REFRESH, HYDRA_SDRAM_AUTO_REFRESH_CYCLES, 0U))) != SDRAM_OK) return status;
    if ((status = Sdram_Command(ops, ctx, (uint32_t)SDRAM_SDCMR(SDRAM_MODE_LOAD_MODE, 1U, SDRAM_MODE_REGISTER_VALUE))) != SDRAM_OK) return status;
    ops->write_reg(ctx, SDRAM_REG_SDRTR, (uint32_t)(SDRAM_REFRESH_COUNT << 1));
    return SDRAM_OK;
}

#endif // HYDRA_PLATFORM_H757

#endif // HYDRA_SDRAM_INIT_H
