/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header (DSI host)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the DSI host of the CM7 (src/mcu_stm32h757/CM7/DsiHost.c) to compile with the host's plain gcc. The names and the fields are the ones of the
 * HAL of the H7; the functions are declared here and defined by the test, which plays the host and records what the driver did.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_DSI_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_DSI_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

typedef struct { int instance_id; } DSI_TypeDef;
typedef struct { uint32_t AutomaticClockLaneControl, TXEscapeCkdiv, NumberOfLanes; } DSI_InitTypeDef;
typedef struct { uint32_t PLLNDIV, PLLIDF, PLLODF; } DSI_PLLInitTypeDef;
typedef struct { DSI_TypeDef *Instance; DSI_InitTypeDef Init; } DSI_HandleTypeDef;

typedef struct {
    uint32_t VirtualChannelID, ColorCoding, LooselyPacked, Mode, PacketSize, NumberOfChunks, NullPacketSize, HSPolarity, VSPolarity, DEPolarity;
    uint32_t HorizontalSyncActive, HorizontalBackPorch, HorizontalLine, VerticalSyncActive, VerticalBackPorch, VerticalFrontPorch, VerticalActive;
    uint32_t LPCommandEnable, LPLargestPacketSize, LPVACTLargestPacketSize;
    uint32_t LPHorizontalFrontPorchEnable, LPHorizontalBackPorchEnable, LPVerticalActiveEnable, LPVerticalFrontPorchEnable, LPVerticalBackPorchEnable, LPVerticalSyncActiveEnable;
    uint32_t FrameBTAAcknowledgeEnable;
} DSI_VidCfgTypeDef;

extern DSI_TypeDef stub_dsi;
#define DSI (&stub_dsi)

#define DSI_PLL_IN_DIV1 0x00000001U
#define DSI_PLL_IN_DIV2 0x00000002U
#define DSI_PLL_IN_DIV3 0x00000003U
#define DSI_PLL_IN_DIV4 0x00000004U
#define DSI_PLL_IN_DIV5 0x00000005U
#define DSI_PLL_IN_DIV6 0x00000006U
#define DSI_PLL_IN_DIV7 0x00000007U
#define DSI_PLL_OUT_DIV1 0x00000000U
#define DSI_PLL_OUT_DIV2 0x00000001U
#define DSI_PLL_OUT_DIV4 0x00000002U
#define DSI_PLL_OUT_DIV8 0x00000003U
#define DSI_AUTO_CLK_LANE_CTRL_DISABLE 0x00000000U
#define DSI_ONE_DATA_LANE 0U
#define DSI_TWO_DATA_LANES 1U
#define DSI_RGB565 0x00000000U
#define DSI_RGB666 0x00000003U
#define DSI_RGB888 0x00000005U
#define DSI_LOOSELY_PACKED_DISABLE 0x00000000U
#define DSI_VID_MODE_NB_PULSES 0x00000000U
#define DSI_HSYNC_ACTIVE_HIGH 0x00000000U
#define DSI_VSYNC_ACTIVE_HIGH 0x00000000U
#define DSI_DATA_ENABLE_ACTIVE_HIGH 0x00000000U
#define DSI_LP_COMMAND_ENABLE 0x00000100U
#define DSI_LP_HFP_ENABLE 0x00002000U
#define DSI_LP_HBP_ENABLE 0x00004000U
#define DSI_LP_VACT_ENABLE 0x00008000U
#define DSI_LP_VFP_ENABLE 0x00001000U
#define DSI_LP_VBP_ENABLE 0x00000800U
#define DSI_LP_VSYNC_ENABLE 0x00000400U
#define DSI_FBTAA_DISABLE 0x00000000U
#define DSI_DCS_SHORT_PKT_WRITE_P0 0x00000005U
#define DSI_DCS_SHORT_PKT_WRITE_P1 0x00000015U
#define DSI_DCS_LONG_PKT_WRITE 0x00000039U

void stub_clock(int which);
#define __HAL_RCC_DSI_CLK_ENABLE() stub_clock(1)
#define __HAL_RCC_DSI_CLK_DISABLE() stub_clock(-1)

HAL_StatusTypeDef HAL_DSI_Init(DSI_HandleTypeDef *hdsi, DSI_PLLInitTypeDef *pll);
HAL_StatusTypeDef HAL_DSI_DeInit(DSI_HandleTypeDef *hdsi);
HAL_StatusTypeDef HAL_DSI_ConfigVideoMode(DSI_HandleTypeDef *hdsi, DSI_VidCfgTypeDef *video);
HAL_StatusTypeDef HAL_DSI_Start(DSI_HandleTypeDef *hdsi);
HAL_StatusTypeDef HAL_DSI_ShortWrite(DSI_HandleTypeDef *hdsi, uint32_t channel, uint32_t mode, uint32_t param1, uint32_t param2);
HAL_StatusTypeDef HAL_DSI_LongWrite(DSI_HandleTypeDef *hdsi, uint32_t channel, uint32_t mode, uint32_t nb_params, uint32_t param1, uint8_t *params);
void HAL_Delay(uint32_t ms);

#endif /* HYDRA_UMC_HOST_STUB_H7_DSI_HAL_H */
