/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the H745 bootloader sources that do not
 * talk to a peripheral directly (bootloader_crypto.c, bootloader_flash.c) to
 * compile with the host's plain gcc. The functions are declared here and
 * defined by the test, which plays the flash controller and the watchdog.
 */
#ifndef HYDRA_UMC_HOST_STUB_STM32H7XX_HAL_H
#define HYDRA_UMC_HOST_STUB_STM32H7XX_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

typedef struct { int unused; } SPI_HandleTypeDef;
typedef struct { int unused; } FDCAN_HandleTypeDef;
typedef struct { int unused; } IWDG_HandleTypeDef;

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

HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase_init, uint32_t *sector_error);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type_program, uint32_t address, uint32_t data_address);
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *hiwdg);

#endif /* HYDRA_UMC_HOST_STUB_STM32H7XX_HAL_H */
