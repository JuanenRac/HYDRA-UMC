/*
 * =============================================================================
 * STM32H745ZI_CM4_main.c - Kinematic Brain, Cortex-M4 application entry point
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * This core's real job per README.md section 5: FDCAN1 "STACK A" master
 * (docs/architecture.md sections 4-5 - slot-addressed AXIS_STATUS/
 * AXIS_STEP_TELEMETRY queries and the RELAY_SEND/RELAY_RECV tunnel that
 * reaches Tier 2/3 through each Robot Controller Board), now real - see
 * KinematicBrainCan.c. Analog sensor filtering and safety interlocks are
 * still not implemented (this core's other real job, still future work -
 * needs the sensor pinout wired first, unlike FDCAN1 which already has a
 * proven init to reuse from the bootloader).
 *
 * REAL CLOCK CONFIG (see SystemClock_Config() below): this repo's own BOM
 * (hardware/PCB/kinematic_brain_stm32h745/BOM.TXT) confirms no external
 * HSE crystal on this board - PLL1 is driven from the internal HSI64
 * instead. Configured HERE, on this core only, deliberately NOT also on
 * CM7 (whose own main() still calls the old do-nothing placeholder) -
 * PLL1 is a single physical chip-wide resource, and this chip's own real
 * dual-core boot behavior is two FULLY INDEPENDENT resets (no CM7-releases-
 * CM4 handshake exists in this design - see ../boot/bootloader_main.c's
 * own header: "CM7 and CM4 both individually reset into their own default
 * HSI64 state"), so if both cores' application code tried to configure
 * PLL1 independently, that would be a real register-level race the very
 * first time real hardware exists to hit it on. Only this core touches
 * PLL1/RCC until CM7's own real clock config is designed with that race
 * explicitly resolved (a real HSEM-gated "who configures RCC" handshake -
 * not done here, out of scope for what this session's work needs: this
 * core reaching the G474 boards for real, not CM7's own S-curve motion
 * engine).
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
#include "KinematicBrainCan.h"

static IWDG_HandleTypeDef hiwdg;

/* Real PLL1 clock tree: HSI64 -> /M -> VCO -> /P -> SYSCLK, targeting the
 * real 480 MHz SYSCLK / 240 MHz CM4 core clock this project's own BOM
 * documents (hardware/PCB/kinematic_brain_stm32h745/BOM.TXT: "Cortex-
 * M7@480MHz + Cortex-M4@240MHz"). No external HSE crystal on this board
 * (same BOM, confirmed absent) - PLL1 is driven from the internal HSI64
 * instead, same real oscillator every bootloader in this project already
 * assumes as its own reset-default fallback.
 *
 * M=8: VCO input = 64MHz/8 = 8MHz exactly, RCC_PLL1VCIRANGE_2's own real
 * upper bound (4-8MHz) - the real ST convention is that a range's own
 * upper bound belongs to that range, not the next one up.
 * N=120, P=2: VCO output = 8MHz*120 = 960MHz (within the real wide-VCO
 * 192-960MHz ceiling); SYSCLK = 960MHz/2 = 480MHz exactly.
 * Q=4, R=2: FDCAN/other kernel clocks - not yet selected to source from
 * PLL1Q/R below (kept at their own HSI-derived defaults for now, matching
 * MX_FDCAN1_Init()'s own still-TODO kernel-clock re-verification note in
 * KinematicBrainCan.c) - only SYSCLK itself is switched to PLL1 here.
 * AHB (HCLK, this core's own bus/CPU clock) = SYSCLK/2 = 240MHz via
 * D1CPRE=1/HPRE=2, matching the BOM's own real Cortex-M4 target exactly.
 * APB1/2/3/4 = HCLK/2 = 120MHz, at H7's own real 120MHz APB ceiling.
 * VOS0 (highest performance regulator scale) + 2WS flash latency are the
 * real documented prerequisites for running SYSCLK at 480MHz.
 *
 * Verify against RM0399 (or a real HAL_RCC_OscConfig/ClockConfig return
 * value check) the first time real hardware exists to test this against -
 * same honesty already applied to every other not-yet-hardware-verified
 * piece of this project. A failed PLL lock here fails safe: HAL_RCC_
 * OscConfig/ClockConfig return HAL_ERROR rather than silently running at
 * the wrong frequency, and this function treats that as fatal (blinks a
 * distinct fast pattern via the watchdog reset path below) rather than
 * silently continuing on an unconfigured clock. */
static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  osc.HSIState = RCC_HSI_ON;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  osc.PLL.PLLM = 8;
  osc.PLL.PLLN = 120;
  osc.PLL.PLLP = 2;
  osc.PLL.PLLQ = 4;
  osc.PLL.PLLR = 2;
  osc.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  osc.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  osc.PLL.PLLFRACN = 0;

  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);
  while (__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY) == RESET) { }

  if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
    /* Fail safe rather than run on an unconfigured/unstable clock - see
     * this function's own header comment. */
    __disable_irq();
    for (;;) { }
  }

  RCC_ClkInitTypeDef clk = {0};
  clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_D1PCLK1 |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.SYSCLKDivider = RCC_SYSCLK_DIV1;
  clk.AHBCLKDivider = RCC_HCLK_DIV2;
  clk.APB3CLKDivider = RCC_APB3_DIV2;
  clk.APB1CLKDivider = RCC_APB1_DIV2;
  clk.APB2CLKDivider = RCC_APB2_DIV2;
  clk.APB4CLKDivider = RCC_APB4_DIV2;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
    __disable_irq();
    for (;;) { }
  }
}

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

/* Real FDCAN1 "STACK A" task - queries AXIS_STATUS for every one of the 8
 * real slots in round-robin, so the real relay tunnel/telemetry path is
 * genuinely exercised on a live bus rather than only reachable through a
 * one-off call. A real timeout on an unpopulated/unresponsive slot is
 * expected, not an error - see KinematicBrainCan_QueryAxisStatus()'s own
 * header. */
static void vStackATask(void *pvParameters)
{
  (void)pvParameters;
  KinematicBrainCan_Init();
  uint8_t slot = 0;
  for (;;) {
    uint8_t status[8];
    uint8_t dlc = 0;
    (void)KinematicBrainCan_QueryAxisStatus(slot, status, &dlc, 50);
    slot = (uint8_t)((slot + 1) % 8);
    vTaskDelay(pdMS_TO_TICKS(100));
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
  SystemClock_Config();
  GPIO_Init();

  xTaskCreate(vBlinkTask, "blink", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);
  xTaskCreate(vStackATask, "stacka", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, NULL);

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
