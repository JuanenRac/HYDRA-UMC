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
 *
 * WATCHDOG NOTE (found auditing this file, not a pre-existing TODO): IWDG2
 * is independent hardware that, once started, survives NVIC_SystemReset()
 * AND a bootloader->application jump alike - HAL_DeInit()/HAL_RCC_DeInit()
 * (called by ../boot/bootloader_protocol.c's own JumpToApplication() right
 * before landing here) do not and cannot touch it. The bootloader
 * (../boot/bootloader_main.c) arms IWDG2 (Prescaler=32, Reload=4095, ~4s
 * timeout at the assumed 32kHz LSI) as literally its first action in
 * main() - so by the time this application's own main() below starts
 * running, that same countdown is already ticking with an unknown amount
 * of budget left, and nothing in this file used to touch IWDG2 at all.
 * Left unrefreshed, real hardware would reset roughly every 4 seconds
 * forever (bootloader re-arms, jumps back here, resets again) - the exact
 * same IWDG_Instance/Prescaler/Reload are reasserted below (re-asserting
 * matching values is a harmless no-op on already-running IWDG hardware)
 * purely so this file has its own local handle to refresh from the blink
 * task, not to change the timeout the bootloader already set.
 * =============================================================================
 */

#include "FreeRTOS.h"
#include "task.h"
#include "stm32h7xx_hal.h"

static IWDG_HandleTypeDef hiwdg;

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
    HAL_IWDG_Refresh(&hiwdg); /* see this file's own header WATCHDOG NOTE - the bootloader's ~4s IWDG2 survives the jump into here unrefreshed otherwise */
    HAL_GPIO_TogglePin(GPIOG, GPIO_PIN_10);
    vTaskDelay(pdMS_TO_TICKS(400));
  }
}

int main(void)
{
  /* Re-arm/refresh this same IWDG2 the bootloader already started (see
   * this file's own header WATCHDOG NOTE) as the very first action here,
   * before HAL_Init() - same "don't leave an already-ticking countdown
   * unattended any longer than necessary" reasoning the bootloader itself
   * applies to its own first line of main(). */
  hiwdg.Instance = IWDG2;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
  hiwdg.Init.Reload = 4095;
  HAL_IWDG_Init(&hiwdg);

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
