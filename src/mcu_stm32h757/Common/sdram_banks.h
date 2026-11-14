// =============================================================================
// HYDRA-UMC Kinematic Brain - which banks of the external SDRAM may be used, with a probe of the optional second bank, header-only and free of the HAL
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// Bank 1 is fitted on the board and bank 2 is optional (HYDRA_SDRAM_BANK2_ENABLED, off by default). With HYDRA_SDRAM_BANK2_PROBE (off by default) the firmware does not take an
// enabled bank 2 on trust: after Sdram_Start has returned SDRAM_OK it runs the memory tests of memtest.h over a small window of bank 2 and switches the bank off when they fail,
// so the system keeps running on bank 1 (the heap, the framebuffer and the DMA buffers are then placed in bank 1 only). The memory is reached through MemOps, so the host test
// runs the decision against a RAM double with faults. A probe destroys the contents of its window: it runs once, before the bank is used.
//   - SdramBanks_Resolve: the decision, from plain numbers (which banks the build enables, whether to probe, the operations on bank 2 and the size of the window in words).
//   - SdramBanks_ResolveBuild / SdramBanks_UsableBytes: the same with the values of the platform profile (H757 only).
// Nothing here has run on a board; the proof on the board is a bank 2 that is missing or damaged and a start that ends on bank 1 alone.
// =============================================================================
#ifndef HYDRA_SDRAM_BANKS_H
#define HYDRA_SDRAM_BANKS_H

#include <stdint.h>
#include "memtest.h"

#ifndef HYDRA_SDRAM_BANK2_PROBE
#define HYDRA_SDRAM_BANK2_PROBE 0 /* 1: probe bank 2 with the memory tests after the start-up and drop it when they fail */
#endif

/* the window of bank 2 that the probe tests, in 32-bit words (a power of two): 64 KiB */
#define SDRAM_BANKS_PROBE_WORDS 16384u

typedef struct {
    uint8_t bank1;          /* 1: bank 1 may be used */
    uint8_t bank2;          /* 1: bank 2 may be used */
    uint8_t bank2_probed;   /* 1: the memory tests ran on bank 2 */
    uint8_t any;            /* 1: at least one bank may be used (0: the firmware must run without external memory) */
    MemtestReport bank2_report; /* what the probe found; status OK when it did not run or passed */
} SdramBanks;

/* `bank2_ops` and `ctx` are used only when bank 2 is enabled and `probe` is not 0; `window_words` is a power of two (a bad window counts as a failed probe). */
static inline void SdramBanks_Resolve(SdramBanks *out, int bank1_enabled, int bank2_enabled, int probe, const MemOps *bank2_ops, void *ctx, uint32_t window_words)
{
    out->bank1 = bank1_enabled ? 1u : 0u;
    out->bank2 = bank2_enabled ? 1u : 0u;
    out->bank2_probed = 0u;
    out->bank2_report.status = MEMTEST_OK;
    out->bank2_report.word_index = 0u;
    out->bank2_report.expected = 0u;
    out->bank2_report.actual = 0u;
    out->bank2_report.element = 0u;
    if (out->bank2 && probe) {
        out->bank2_probed = 1u;
        if (Memtest_Run(bank2_ops, ctx, window_words, &out->bank2_report) != MEMTEST_OK) out->bank2 = 0u;
    }
    out->any = (out->bank1 || out->bank2) ? 1u : 0u;
}

#if defined(HYDRA_PLATFORM_H757)
#include "platform_profile.h"

/* The decision with the banks and the probe switch of this build. */
static inline void SdramBanks_ResolveBuild(SdramBanks *out, const MemOps *bank2_ops, void *ctx)
{
    SdramBanks_Resolve(out, HYDRA_SDRAM_BANK1_ENABLED, HYDRA_SDRAM_BANK2_ENABLED, HYDRA_SDRAM_BANK2_PROBE, bank2_ops, ctx, SDRAM_BANKS_PROBE_WORDS);
}

/* The bytes the firmware may use after the decision: the window of each bank that survived. */
static inline uint32_t SdramBanks_UsableBytes(const SdramBanks *banks)
{
    return (banks->bank1 ? (uint32_t)HYDRA_SDRAM_BANK1_SIZE_BYTES : 0u) + (banks->bank2 ? (uint32_t)HYDRA_SDRAM_BANK2_SIZE_BYTES : 0u);
}
#endif // HYDRA_PLATFORM_H757

#endif // HYDRA_SDRAM_BANKS_H
