/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32G4 HAL header
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the bootloader sources that do not talk
 * to a peripheral directly (bootloader_crypto.c, bootloader_flash.c) to compile
 * with the host's plain gcc. The functions are declared here and defined by the
 * test, which plays the flash controller and the watchdog.
 */
#ifndef HYDRA_UMC_HOST_STUB_STM32G4XX_HAL_H
#define HYDRA_UMC_HOST_STUB_STM32G4XX_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

typedef struct { int unused; } FDCAN_HandleTypeDef;
typedef struct { int unused; } IWDG_HandleTypeDef;

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

HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase_init, uint32_t *page_error);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type_program, uint32_t address, uint64_t data);
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *hiwdg);

#endif /* HYDRA_UMC_HOST_STUB_STM32G4XX_HAL_H */
