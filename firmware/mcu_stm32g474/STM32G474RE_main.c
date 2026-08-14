/*
 * =============================================================================
 * STM32G474RE_main.c - Robot Controller Board application entry point
 * PROJECT: HYDRA-UMC (Robot Controller Board firmware, Tier 1 - STM32G474RET6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - proves the toolchain/HAL/linker setup documented in
 * docs/COMPILE_STM32G474.TXT actually produces a working binary (a GPIO
 * toggle, the traditional "does this chip even boot" smoke test), not a
 * real robot-controller-board application yet. Real work still needed here,
 * tracked against HYDRA-UMC/docs/architecture.md:
 *   - FDCAN1 init + the Tier 1 slot-addressed protocol (architecture.md §3)
 *   - FDCAN2 (or 3) init + the Tier 2/3 relay tunnel (architecture.md §4)
 *   - 6x STEP/DIR/EN outputs + endstop inputs (this board's actual job)
 *   - Real clock tree config (left at HSI 16 MHz default below - fine for
 *     this smoke test, NOT fine for real FDCAN bit timing or step-pulse
 *     precision)
 *   - The bootloader-facing side of the metadata/main/backup flash split
 *     defined in STM32G474RETx_APP.ld's own header comment
 *
 * PLACEHOLDER PIN: PA5 (matches common Nucleo-G474RE dev-board LED wiring) -
 * this repo's own hardware/PCB/robot_controller_board_stm32g474/ doesn't
 * have a real schematic yet (see that folder's own README) - adjust once
 * one exists.
 * =============================================================================
 */

#include "stm32g4xx_hal.h"

static void Error_Handler(void)
{
  __disable_irq();
  while (1) { }
}

static void GPIO_Init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gpio);
}

int main(void)
{
  HAL_StatusTypeDef hal_status = HAL_Init();
  if (hal_status != HAL_OK) {
    Error_Handler();
  }

  /* Clock tree left at HSI (16 MHz) reset default - a smoke-test-only
   * choice. Real FDCAN bit timing and step-pulse generation both need a
   * proper SystemClock_Config() (HSE + PLL) written once the board's real
   * crystal/oscillator choice is known - see this file's own header. */

  GPIO_Init();

  while (1) {
    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
    HAL_Delay(500);
  }
}

void HAL_MspInit(void)
{
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();
}

void NMI_Handler(void) { }
void HardFault_Handler(void) { while (1) { } }
void MemManage_Handler(void) { while (1) { } }
void BusFault_Handler(void) { while (1) { } }
void UsageFault_Handler(void) { while (1) { } }
void SVC_Handler(void) { }
void DebugMon_Handler(void) { }
void PendSV_Handler(void) { }

static volatile uint32_t g_tick = 0;
void SysTick_Handler(void)
{
  g_tick++;
  HAL_IncTick();
}
