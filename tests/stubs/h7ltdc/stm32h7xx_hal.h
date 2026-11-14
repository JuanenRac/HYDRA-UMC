/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 header (LTDC and DMA2D registers)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the LTDC and DMA2D driver (src/mcu_stm32h757/CM7/DisplayLtdc.c) to compile with the host's plain gcc. The peripherals are plain structures
 * in memory, defined by the test; the register layout and the bit positions are the ones of the reference manual that the driver uses.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_LTDC_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_LTDC_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    volatile uint32_t SSCR, BPCR, AWCR, TWCR, GCR, SRCR, IER, ISR, ICR;
} LTDC_TypeDef;

typedef struct {
    volatile uint32_t CR, WHPCR, WVPCR, CKCR, PFCR, CACR, DCCR, BFCR, CFBAR, CFBLR, CFBLNR;
} LTDC_Layer_TypeDef;

typedef struct {
    volatile uint32_t CR, ISR, IFCR, FGMAR, FGOR, BGMAR, BGOR, FGPFCCR, FGCOLR, BGPFCCR, BGCOLR, OPFCCR, OCOLR, OMAR, OOR, NLR;
} DMA2D_TypeDef;

extern LTDC_TypeDef stub_ltdc;
extern LTDC_Layer_TypeDef stub_ltdc_layer1;
extern DMA2D_TypeDef stub_dma2d;
#define LTDC (&stub_ltdc)
#define LTDC_Layer1 (&stub_ltdc_layer1)
#define DMA2D (&stub_dma2d)

#define LTDC_GCR_LTDCEN 0x00000001UL
#define LTDC_SRCR_IMR 0x00000001UL
#define LTDC_LxCR_LEN 0x00000001UL
#define LTDC_BFCR_CONSTANT_ALPHA 0x00000405UL /* BF1 = 4 (constant alpha), BF2 = 5 */

#define DMA2D_ISR_TEIF 0x00000001UL
#define DMA2D_ISR_TCIF 0x00000002UL
#define DMA2D_ISR_CEIF 0x00000020UL
#define DMA2D_IFCR_ALL 0x0000003FUL

void stub_clock_enable(int which);
#define __HAL_RCC_LTDC_CLK_ENABLE() stub_clock_enable(1)
#define __HAL_RCC_DMA2D_CLK_ENABLE() stub_clock_enable(2)

#endif /* HYDRA_UMC_HOST_STUB_H7_LTDC_HAL_H */
