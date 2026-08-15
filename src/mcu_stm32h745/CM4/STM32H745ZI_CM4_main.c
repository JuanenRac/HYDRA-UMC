/*
 * =============================================================================
 * STM32H745ZI_CM4_main.c - Kinematic Brain, Cortex-M4 application entry point
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - see ../CM7/STM32H745ZI_CM7_main.c's own header for
 * the shared dual-core-bring-up reasoning. This core's real job per
 * README.md section 5: FDCAN1 protocol management (the Tier 1
 * slot-addressed bootloader/relay scheme from docs/architecture.md sections
 * 3-4), analog sensor filtering, safety interlocks, inter-core IPC with
 * CM7. None of that exists here yet - this is a standalone FreeRTOS
 * GPIO-toggle smoke test only, and does not currently wait for or
 * synchronize with CM7 in any way (no HSEM use) - real dual-core bring-up
 * needs that on both sides, not just this one.
 *
 * PIN: PG10 - one of the 3 explicitly-spare GPIOs in this chip's real
 * pinout (docs/PINOUT_STM32H745_KINEMATIC_BRAIN.TXT section 9c) - PE0 (the
 * previous placeholder) is now real hardware: PUMP1 (see that pinout
 * file's section 6). This core's REAL peripherals - SPI1 to the CM5,
 * FDCAN1 to STACK A - are no longer unimplemented: ../boot/bootloader_
 * main.c already initializes both for real (the CAN-OTA gateway). This
 * application entry point just hasn't caught up to reuse that same init
 * yet - see this file's own "real work still needed" note above.
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
  __HAL_RCC_GPIOG_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = GPIO_PIN_10;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &gpio);
}

static void vBlinkTask(void *pvParameters)
{
  (void)pvParameters;
  for (;;) {
    HAL_GPIO_TogglePin(GPIOG, GPIO_PIN_10);
    vTaskDelay(pdMS_TO_TICKS(400));
  }
}

int main(void)
{
  HAL_Init();
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
