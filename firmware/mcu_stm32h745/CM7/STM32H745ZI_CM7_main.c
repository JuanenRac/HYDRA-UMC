/*
 * =============================================================================
 * STM32H745ZI_CM7_main.c - Kinematic Brain, Cortex-M7 application entry point
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - proves the toolchain/HAL/linker/FreeRTOS setup
 * documented in docs/COMPILE_STM32H745.TXT actually produces a working CM7
 * binary (a single FreeRTOS task toggling a GPIO), not the real
 * motion-engine firmware yet. Real work still needed here, tracked against
 * docs/architecture.md:
 *   - Real dual-core bring-up: D1/D2/D3 domain power sequencing, CM4 boot
 *     release (this chip's own BCM4 option byte / RCC_GetBootCM4() dance),
 *     HSEM (hardware semaphore) init for CM7<->CM4 synchronization - NONE
 *     of that is done here yet; this core boots standalone and never
 *     touches CM4 at all
 *   - I/D-cache enable (SCB_EnableICache/DCache) - matters a lot more on
 *     this core than a Cortex-M4 skeleton, left off here for a first smoke
 *     test's simplicity
 *   - Real tasks: S-curve kinematic engine, hardware timer pulse generation
 *     (this core's actual job per README.md section 5), SPI1 slave-mode IPC
 *     to the CM5 - vBlinkTask below is a placeholder for where those tasks
 *     will live, not itself one of them
 *   - Real clock tree config (PLL for 480 MHz) - left at HSI default below;
 *     update FreeRTOSConfig.h's own configCPU_CLOCK_HZ in the same commit
 *     that fixes this, or every FreeRTOS tick/delay silently goes wrong
 *
 * PLACEHOLDER PIN: PB0 - no real schematic exists yet for this board
 * either (see hardware/PCB/kinematic_brain_stm32h745/README.md) - adjust
 * once one does.
 *
 * FreeRTOS note: SVC_Handler/PendSV_Handler/SysTick_Handler are deliberately
 * NOT defined in this file - see FreeRTOSConfig.h's own header comment.
 * =============================================================================
 */

#include "FreeRTOS.h"
#include "task.h"
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

static void vBlinkTask(void *pvParameters)
{
  (void)pvParameters;
  for (;;) {
    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

int main(void)
{
  HAL_Init();

  /* Real clock tree (HSE + PLL1 to 480 MHz) and dual-core bring-up (CM4
   * release, D3 domain, HSEM) both still TODO - see this file's own header.
   * Left at HSI default for this smoke test. */

  GPIO_Init();

  xTaskCreate(vBlinkTask, "blink", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);

  vTaskStartScheduler();

  /* Only reached if vTaskStartScheduler() itself failed. */
  __disable_irq();
  for (;;) { }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask; (void)pcTaskName;
  __disable_irq();
  while (1) { }
}

void vApplicationMallocFailedHook(void)
{
  __disable_irq();
  while (1) { }
}

/* FreeRTOS now owns SysTick - HAL_IncTick() has to be driven from somewhere
 * else, or HAL_GetTick()/HAL_Delay() silently stop advancing. See
 * ../../mcu_stm32g474/STM32G474RE_main.c's own note on this same gotcha. */
void vApplicationTickHook(void)
{
  HAL_IncTick();
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
