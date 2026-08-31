/*
 * =============================================================================
 * STM32G474RE_main.c - Robot Controller Board application entry point
 * PROJECT: HYDRA-UMC (Robot Controller Board firmware, Tier 1 - STM32G474RET6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * Real now: FDCAN1 (uplink, STACK A) responding to real AXIS_STATUS
 * queries with real endstop/fault GPIO state, and FDCAN2 (downlink) running
 * the real Tier 2/3 RELAY_SEND/RELAY_RECV tunnel to this robot's own URTC
 * Tool Head - see RobotControllerRelay.c, the app-side relay logic
 * docs/architecture.md section 6's own status table used to flag as "not
 * yet written". vBlinkTask below is still just the toolchain/HAL/linker/
 * FreeRTOS smoke test (does this chip even boot, does the scheduler
 * actually run) - vRelayTask is the first REAL task. Still needed here,
 * tracked against HYDRA-UMC/docs/architecture.md:
 *   - 6-axis STEP/DIR/EN pulse generation (architecture.md's own "real
 *     tasks" list) - no motion-control code exists yet, only the relay/
 *     status responder above
 *   - OFS_ENTER_BOOTLOADER handling - see RobotControllerRelay.h's own
 *     header for why this is a real, separately-tracked gap
 *   - Real clock tree config (left at HSI 16 MHz default below - fine for
 *     this smoke test, NOT fine for real FDCAN bit timing or step-pulse
 *     precision) - FreeRTOSConfig.h's own configCPU_CLOCK_HZ must be
 *     updated in the SAME commit as whichever change fixes this, or every
 *     FreeRTOS tick/delay silently goes wrong
 *   - The bootloader-facing side of the metadata/main/backup flash split
 *     defined in STM32G474RETx_APP.ld's own header comment
 *
 * WATCHDOG NOTE (found auditing this file, not a pre-existing TODO): IWDG
 * is independent hardware that, once started, survives NVIC_SystemReset()
 * AND a bootloader->application jump alike - HAL_DeInit()/HAL_RCC_DeInit()
 * (called by ../boot/bootloader_protocol.c's own JumpToApplication()
 * right before landing here) do not and cannot touch it. The bootloader
 * (../boot/bootloader_main.c) arms IWDG (Prescaler=32, Reload=999, ~1s
 * timeout at the assumed 32kHz LSI) as literally its first action in
 * main() - so by the time this application's own main() below starts
 * running, that same countdown is already ticking with an unknown amount
 * of budget left, and nothing in this file used to touch IWDG at all.
 * Left unrefreshed, real hardware would reset roughly once a second
 * forever (bootloader re-arms, jumps back here, resets again) - the exact
 * same IWDG_Instance/Prescaler/Reload are reasserted below (re-asserting
 * matching values is a harmless no-op on already-running IWDG hardware)
 * purely so this file has its own local handle to refresh from the blink
 * task, not to change the timeout the bootloader already set.
 *

 * PIN: PC13, STATUS_LED - a REAL pinout allocation now, not a Nucleo-dev-
 * board placeholder (see docs/PINOUT_STM32G474_ROBOT_CONTROLLER.TXT
 * section 4). The chip's other real peripherals (FDCAN1/2, 6x TMC5160A
 * SPI daisy-chain + STEP/DIR/EN, 6x endstops, BOARD_ID DIP switch) are all
 * pinned out in that same file but NOT yet initialized here - that's the
 * "real tasks" work listed below, still not done. The bootloader
 * (../boot/) DOES already initialize FDCAN1 for real - see that folder's
 * own bootloader_main.c if you need a working reference for this file's
 * own eventual FDCAN1 init.
 *
 * FreeRTOS note: SVC_Handler/PendSV_Handler/SysTick_Handler are deliberately
 * NOT defined in this file - FreeRTOSConfig.h renames FreeRTOS's own port.c
 * handlers onto those exact symbol names instead (see that file's own
 * header comment). Defining them here too would be a duplicate-symbol link
 * error.
 * =============================================================================
 */

#include "FreeRTOS.h"
#include "task.h"
#include "stm32g4xx_hal.h"
#include "RobotControllerRelay.h"

static void Error_Handler(void)
{
  __disable_irq();
  while (1) { }
}

static IWDG_HandleTypeDef hiwdg;

static void GPIO_Init(void)
{
  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = GPIO_PIN_13;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &gpio);
}

static void vBlinkTask(void *pvParameters)
{
  (void)pvParameters;
  for (;;) {
    HAL_IWDG_Refresh(&hiwdg); /* see this file's own header WATCHDOG NOTE - the bootloader's ~1s IWDG survives the jump into here unrefreshed otherwise */
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

/* Real task: services this board's own AXIS_STATUS/RELAY_SEND/RELAY_RECV
 * traffic on FDCAN1 and buffers URTC-head traffic off FDCAN2 - see
 * RobotControllerRelay.c's own header for the full real design. Polled
 * every 5ms rather than blocking-per-frame: this task also has to keep
 * servicing FDCAN2 capture even when FDCAN1 has nothing queued, so a
 * short, bounded delay between passes (not vTaskDelay(0), which would
 * starve lower-priority tasks) is the right real scheduling shape here,
 * not a single-peripheral blocking wait. */
static void vRelayTask(void *pvParameters)
{
  (void)pvParameters;
  for (;;) {
    RobotControllerRelay_Poll();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

int main(void)
{
  /* Re-arm/refresh this same IWDG the bootloader already started (see this
   * file's own header WATCHDOG NOTE) as the very first action here, before
   * HAL_Init() - same "don't leave an already-ticking countdown
   * unattended any longer than necessary" reasoning the bootloader itself
   * applies to its own first line of main(). */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
  hiwdg.Init.Reload = 999;
  HAL_IWDG_Init(&hiwdg);

  HAL_StatusTypeDef hal_status = HAL_Init();
  if (hal_status != HAL_OK) {
    Error_Handler();
  }

  /* Clock tree left at HSI (16 MHz) reset default - a smoke-test-only
   * choice. Real FDCAN bit timing and step-pulse generation both need a
   * proper SystemClock_Config() (HSE + PLL) written once the board's real
   * crystal/oscillator choice is known - see this file's own header, and
   * update FreeRTOSConfig.h's own configCPU_CLOCK_HZ in the same commit. */

  GPIO_Init();
  RobotControllerRelay_Init();

  xTaskCreate(vBlinkTask, "blink", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);
  xTaskCreate(vRelayTask, "relay", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 2, NULL);

  vTaskStartScheduler();

  /* Only reached if vTaskStartScheduler() itself failed (e.g. out of heap
   * for the idle/timer tasks) - not a normal code path. */
  Error_Handler();
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

/* FreeRTOS now owns SysTick (see this file's own header comment on the
 * handler rename in FreeRTOSConfig.h) - HAL_IncTick() has to be driven from
 * somewhere else, or HAL_GetTick()/HAL_Delay() silently stop advancing.
 * configUSE_TICK_HOOK=1 in FreeRTOSConfig.h calls this once per tick. */
void vApplicationTickHook(void)
{
  HAL_IncTick();
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
