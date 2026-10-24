/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header, with the core, mailbox and CAN pieces the H745 protocol sources use
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * tests/stubs/stm32h7xx_hal.h is enough for the flash and crypto sources. The protocol sources of the two H745 bootloaders (the update
 * state machine of the CM4 over SPI/CAN and of the CM7 over the shared-memory mailbox) also read the tick, take hardware semaphores, send CAN
 * frames and can jump to the application; this header declares those so the sources compile with the host's plain gcc. It shadows the smaller
 * header when its directory comes first on the include path; it does not include it. The tests define the functions.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7PROTOCOL_STM32H7XX_HAL_H
#define HYDRA_UMC_HOST_STUB_H7PROTOCOL_STM32H7XX_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

typedef struct { volatile uint32_t ECR; } FDCAN_GlobalTypeDef;
typedef struct { FDCAN_GlobalTypeDef *Instance; } FDCAN_HandleTypeDef;
typedef struct { int unused; } SPI_HandleTypeDef;
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
#define FDCAN1_IT0_IRQn      19

uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *hfdcan);
HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *hfdcan, FDCAN_TxHeaderTypeDef *header, uint8_t *data);
uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef *hfdcan, uint32_t fifo);
HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(FDCAN_HandleTypeDef *hfdcan, uint32_t fifo, FDCAN_RxHeaderTypeDef *header, uint8_t *data);
HAL_StatusTypeDef HAL_FDCAN_DeInit(FDCAN_HandleTypeDef *hfdcan);

typedef struct {
    uint32_t TypeErase;
    uint32_t Banks;
    uint32_t Sector;
    uint32_t NbSectors;
    uint32_t VoltageRange;
} FLASH_EraseInitTypeDef;

#define FLASH_SECTOR_SIZE           0x00020000UL /* 128 KB */
#define FLASH_TYPEERASE_SECTORS     0x00U
#define FLASH_BANK_1                0x01U
#define FLASH_BANK_2                0x02U
#define FLASH_VOLTAGE_RANGE_3       0x02U
#define FLASH_TYPEPROGRAM_FLASHWORD 0x01U
#define __DSB() ((void)0)
#define __ISB() ((void)0)

HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase_init, uint32_t *sector_error);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type_program, uint32_t address, uint32_t data_address);
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *hiwdg);

#define D1_AXISRAM_BASE 0x24000000UL
#define D2_AHBSRAM_BASE 0x30000000UL
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t ms);
void HAL_NVIC_DisableIRQ(int irq);
void HAL_RCC_DeInit(void);
void HAL_DeInit(void);
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *hspi);

uint32_t HAL_HSEM_IsSemTaken(uint32_t sem_id);
uint32_t HAL_HSEM_FastTake(uint32_t sem_id);
void HAL_HSEM_Release(uint32_t sem_id, uint32_t process_id);

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

#endif /* HYDRA_UMC_HOST_STUB_H7PROTOCOL_STM32H7XX_HAL_H */
