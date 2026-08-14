/*
 * =============================================================================
 * bootloader_main.c - Kinematic Brain, Cortex-M4 bootloader entry point
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - see ../../CM7/boot/bootloader_main.c's own header
 * for the shared reasoning. This core's own bootloader is a simpler
 * "verify CM7 already updated us, then jump" role in most dual-core H7
 * designs (the SPI-OTA conversation with the CM5 happens on the CM7 side,
 * see ../../CM7/boot/), but that coordination isn't implemented yet either
 * - this jumps straight to the CM4 app slot unconditionally, same
 * placeholder behavior as every other bootloader skeleton in this repo.
 * =============================================================================
 */

#include "stm32h7xx_hal.h"

int main(void)
{
  HAL_Init();

  typedef void (*AppEntry)(void);
  uint32_t app_stack = *(volatile uint32_t *)0x08111000U;
  uint32_t app_reset = *(volatile uint32_t *)0x08111004U;

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
