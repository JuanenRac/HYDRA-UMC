/*
 * =============================================================================
 * STM32H745ZI_CM7_main.c - Kinematic Brain, Cortex-M7 application entry point
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - proves the toolchain/HAL/linker setup documented in
 * docs/COMPILE_STM32H745.TXT actually produces a working CM7 binary (a GPIO
 * toggle), not the real motion-engine firmware yet. Real work still needed
 * here, tracked against docs/architecture.md:
 *   - Real dual-core bring-up: D1/D2/D3 domain power sequencing, CM4 boot
 *     release (this chip's own BCM4 option byte / RCC_GetBootCM4() dance),
 *     HSEM (hardware semaphore) init for CM7<->CM4 synchronization - NONE
 *     of that is done here yet; this file boots standalone and never
 *     touches CM4 at all
 *   - I/D-cache enable (SCB_EnableICache/DCache) - matters a lot more on
 *     this core than a Cortex-M4 skeleton, left off here for a first smoke
 *     test's simplicity
 *   - S-curve kinematic engine + hardware timer pulse generation (this
 *     core's actual job per README.md section 5)
 *   - SPI1 slave-mode IPC to the CM5 (README.md section 10) - the
 *     HYDRA_DATA_READY handshake and the Tier-0 SPI-OTA bootloader command
 *     vocabulary from docs/architecture.md section 2
 *   - Real clock tree config (PLL for 480 MHz) - left at HSI default below
 *
 * PLACEHOLDER PIN: PB0 - no real schematic exists yet for this board
 * either (see hardware/PCB/kinematic_brain_stm32h745/README.md) - adjust
 * once one does.
 * =============================================================================
 */

#include "stm32h7xx_hal.h"

static void GPIO_Init(void)
{
  __HAL_RCC_GPIOB_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = GPIO_PIN_0;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &gpio);
}

int main(void)
{
  HAL_Init();

  /* Real clock tree (HSE + PLL1 to 480 MHz) and dual-core bring-up (CM4
   * release, D3 domain, HSEM) both still TODO - see this file's own header.
   * Left at HSI default for this smoke test. */

  GPIO_Init();

  while (1) {
    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);
    HAL_Delay(250);
  }
}

void HAL_MspInit(void)
{
  __HAL_RCC_SYSCFG_CLK_ENABLE();
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
