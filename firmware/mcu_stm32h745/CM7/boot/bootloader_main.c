/*
 * =============================================================================
 * bootloader_main.c - Kinematic Brain, Cortex-M7 SPI-OTA bootloader entry point
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - proves this slot's own linker script/flash region
 * (see ../STM32H745ZITx_CM7_BOOTLOADER.ld) actually links into a bootable
 * image; does NOT implement the SPI-OTA protocol yet. Real bootloader work
 * needed here, tracked against HYDRA-UMC/docs/architecture.md section 2:
 * the same command vocabulary URTC's own bootloader proves out
 * (ENTER_BOOTLOADER, START_UPDATE, DATA, PAGE_ACK, END_UPDATE, STATUS,
 * HEARTBEAT, HMAC_CHUNK, VERSION_QUERY/RESPONSE) carried as a frame-type
 * byte inside the existing SPI1 128-byte telemetry frame (README.md
 * section 10), not a CAN ID - this is the one tier reached over SPI, not
 * CAN, since it's wired directly to the CM5.
 * =============================================================================
 */

#include "stm32h7xx_hal.h"

int main(void)
{
  HAL_Init();

  /* TODO: real SPI-OTA bootloader logic - see this file's own header. For
   * now: jump straight to the CM7 application main slot (0x08011000, see
   * ../STM32H745ZITx_CM7_APP.ld) so a flashed app actually runs. */
  typedef void (*AppEntry)(void);
  uint32_t app_stack = *(volatile uint32_t *)0x08011000U;
  uint32_t app_reset = *(volatile uint32_t *)0x08011004U;

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
