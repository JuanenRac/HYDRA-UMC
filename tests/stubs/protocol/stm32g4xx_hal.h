/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32G4 HAL header, with the CAN and core pieces bootloader_protocol.c uses
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * tests/stubs/stm32g4xx_hal.h is enough for the flash and crypto sources. The protocol source (the CAN-OTA state machine) also sends and
 * receives CAN frames, reads the tick and can jump to the application; this header declares those so it compiles with the host's plain
 * gcc, and tests/test_bootloader_g474_update_host.c defines them (a CAN bus that records what the bootloader sends, a flash model,
 * a clock). It shadows the smaller header when its directory comes first on the include path; it does not include it.
 */
#ifndef HYDRA_UMC_HOST_STUB_PROTOCOL_STM32G4XX_HAL_H
#define HYDRA_UMC_HOST_STUB_PROTOCOL_STM32G4XX_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

typedef struct { volatile uint32_t ECR; } FDCAN_GlobalTypeDef;
typedef struct { FDCAN_GlobalTypeDef *Instance; } FDCAN_HandleTypeDef;
typedef struct { int unused; } IWDG_HandleTypeDef;

typedef struct {
    uint32_t Identifier;
    uint32_t IdType;
    uint32_t TxFrameType;
    uint32_t DataLength;
    uint32_t ErrorStateIndicator;
    uint32_t BitRateSwitch;
    uint32_t FDFormat;
    uint32_t TxEventFifoControl;
    uint32_t MessageMarker;
} FDCAN_TxHeaderTypeDef;

typedef struct {
    uint32_t Identifier;
    uint32_t IdType;
    uint32_t RxFrameType;
    uint32_t DataLength;
} FDCAN_RxHeaderTypeDef;

#define FDCAN_STANDARD_ID    0U
#define FDCAN_DATA_FRAME     0U
#define FDCAN_ESI_ACTIVE     0U
#define FDCAN_BRS_OFF        0U
#define FDCAN_CLASSIC_CAN    0U
#define FDCAN_NO_TX_EVENTS   0U
#define FDCAN_RX_FIFO0       0U
#define FDCAN1_IT0_IRQn      21

uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *hfdcan);
HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *hfdcan, FDCAN_TxHeaderTypeDef *header, uint8_t *data);
uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef *hfdcan, uint32_t fifo);
HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(FDCAN_HandleTypeDef *hfdcan, uint32_t fifo, FDCAN_RxHeaderTypeDef *header, uint8_t *data);
HAL_StatusTypeDef HAL_FDCAN_DeInit(FDCAN_HandleTypeDef *hfdcan);

typedef struct {
    uint32_t TypeErase;
    uint32_t Banks;
    uint32_t Page;
    uint32_t NbPages;
} FLASH_EraseInitTypeDef;

typedef struct { volatile uint32_t OPTR; } FLASH_TypeDef;
extern FLASH_TypeDef host_flash_registers;
#define FLASH (&host_flash_registers)

#define FLASH_OPTR_DBANK            (1UL << 22)
#define FLASH_PAGE_SIZE             2048U
#define FLASH_TYPEERASE_PAGES       0x00U
#define FLASH_BANK_1                0x01U
#define FLASH_BANK_2                0x02U
#define FLASH_TYPEPROGRAM_DOUBLEWORD 0x01U
#define FLASH_FLAG_ALL_ERRORS       0x0000C3FAU
#define __HAL_FLASH_CLEAR_FLAG(flag) ((void)(flag))
#define __DSB() ((void)0)
#define __ISB() ((void)0)

HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase_init, uint32_t *page_error);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type_program, uint32_t address, uint64_t data);
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *hiwdg);

#define SRAM_BASE 0x20000000UL
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t ms);
void HAL_NVIC_DisableIRQ(int irq);
void HAL_RCC_DeInit(void);
void HAL_DeInit(void);

typedef struct { volatile uint32_t MODER; } GPIO_TypeDef;
extern GPIO_TypeDef host_gpioa, host_gpiob;
#define GPIOA (&host_gpioa)
#define GPIOB (&host_gpiob)

typedef struct { volatile uint32_t CTRL, LOAD, VAL; } SysTick_Type;
extern SysTick_Type host_systick;
#define SysTick (&host_systick)

typedef struct { volatile uint32_t ICSR, VTOR; } SCB_Type;
extern SCB_Type host_scb;
#define SCB (&host_scb)
#define SCB_ICSR_PENDSTCLR_Msk (1UL << 25)

void __disable_irq(void);
void __set_MSP(uint32_t top_of_main_stack);
void NVIC_SystemReset(void);

#endif /* HYDRA_UMC_HOST_STUB_PROTOCOL_STM32G4XX_HAL_H */
