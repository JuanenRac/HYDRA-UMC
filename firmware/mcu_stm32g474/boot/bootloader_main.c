/*
 * =============================================================================
 * bootloader_main.c - Robot Controller Board CAN-OTA bootloader entry point
 * PROJECT: HYDRA-UMC (Robot Controller Board firmware, Tier 1 - STM32G474RET6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - proves this slot's own linker script/flash region
 * (see ../STM32G474RETx_BOOTLOADER.ld) actually links into a bootable image;
 * does NOT implement the CAN-OTA protocol yet. The real bootloader needs to
 * be lifted from URTC's own proven implementation (sibling repo, src/F303-
 * master/boot/bootloader_*.c) and re-targeted at:
 *   - FDCAN1 instead of bxCAN, using the Tier-1 slot-addressed ID map
 *     defined in HYDRA-UMC/docs/architecture.md section 3 (offsets +0x00
 *     through +0x0F mirror URTC's own 0x7F0-0x7FF verbatim, so URTC's own
 *     command handling logic - ENTER_BOOTLOADER, START_UPDATE, DATA,
 *     PAGE_ACK, END_UPDATE, STATUS, HEARTBEAT, HMAC_CHUNK, VERSION_QUERY/
 *     RESPONSE, TEC/REC, ALLOW_DOWNGRADE, BACKUP_READ - ports over with
 *     mostly ID re-basing, not a redesign)
 *   - This chip's own flash geometry (512 KB, dual-bank-capable) instead of
 *     the F303's 256 KB single bank - HAL_FLASH_Unlock/Program page-erase
 *     granularity differs, see docs/COMPILE_STM32G474.TXT
 *   - The same CRC32 + HMAC-SHA256 verify-into-backup-before-copy-to-main
 *     discipline URTC's own bootloader already implements (anti-bricking -
 *     do not skip this when porting the real logic in)
 * =============================================================================
 */

#include "stm32g4xx_hal.h"

static void Error_Handler(void)
{
  __disable_irq();
  while (1) { }
}

int main(void)
{
  if (HAL_Init() != HAL_OK) {
    Error_Handler();
  }

  /* TODO: real bootloader logic - see this file's own header comment.
   * For now: jump straight to the application main slot (0x08009000, see
   * ../STM32G474RETx_APP.ld) so a flashed app actually runs, same
   * "bootloader present but transparent until real update logic exists"
   * behavior a from-scratch bring-up needs before CAN-OTA itself works. */
  typedef void (*AppEntry)(void);
  uint32_t app_stack = *(volatile uint32_t *)0x08009000U;
  uint32_t app_reset = *(volatile uint32_t *)0x08009004U;

  __set_MSP(app_stack);
  AppEntry app_entry = (AppEntry)app_reset;
  app_entry();

  while (1) { } /* unreachable */
}

void NMI_Handler(void) { }
void HardFault_Handler(void) { while (1) { } }
void MemManage_Handler(void) { while (1) { } }
void BusFault_Handler(void) { while (1) { } }
void UsageFault_Handler(void) { while (1) { } }
void SVC_Handler(void) { }
void DebugMon_Handler(void) { }
void PendSV_Handler(void) { }
void SysTick_Handler(void) { HAL_IncTick(); }
