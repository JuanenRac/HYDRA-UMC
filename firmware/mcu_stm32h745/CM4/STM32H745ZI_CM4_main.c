/*
 * =============================================================================
 * STM32H745ZI_CM4_main.c - Kinematic Brain, Cortex-M4 application entry point
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - see ../CM7/STM32H745ZI_CM7_main.c's own header for
 * the shared reasoning. This core's real job per README.md section 5:
 * FDCAN1 protocol management (the Tier 1 slot-addressed bootloader/relay
 * scheme from docs/architecture.md sections 3-4), analog sensor filtering,
 * safety interlocks, inter-core IPC with CM7. None of that exists here yet
 * - this is a standalone GPIO-toggle smoke test only, and does not
 * currently wait for or synchronize with CM7 in any way (no HSEM use) -
 * real dual-core bring-up needs that on both sides, not just this one.
 *
 * PLACEHOLDER PIN: PE0 - no real schematic exists yet (see
 * hardware/PCB/kinematic_brain_stm32h745/README.md) - adjust once one does.
 * =============================================================================
 */

#include "stm32h7xx_hal.h"

static void GPIO_Init(void)
{
  __HAL_RCC_GPIOE_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = GPIO_PIN_0;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &gpio);
}

int main(void)
{
  HAL_Init();
  GPIO_Init();

  while (1) {
    HAL_GPIO_TogglePin(GPIOE, GPIO_PIN_0);
    HAL_Delay(400);
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
