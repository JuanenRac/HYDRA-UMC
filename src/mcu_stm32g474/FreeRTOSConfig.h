/*
 * =============================================================================
 * FreeRTOSConfig.h - Robot Controller Board (STM32G474RET6)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * See docs/architecture.md for why this board runs FreeRTOS. Only the
 * application links this (bootloader stays bare-metal, no scheduler - see
 * boot/bootloader_main.c's own header comment).
 *
 * configCPU_CLOCK_HZ is set to this chip's HSI reset default (16 MHz) - NOT
 * a real target clock, because SystemClock_Config() itself isn't
 * implemented yet (see STM32G474RE_main.c's own header comment). Update
 * this the same day real PLL-based clock config lands, or every FreeRTOS
 * tick/delay will silently be wrong.
 * =============================================================================
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configUSE_PREEMPTION                     1
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      1 /* drives vApplicationTickHook() -> HAL_IncTick(), see the .c file's own header comment - without this, HAL_GetTick()/HAL_Delay() silently break once FreeRTOS owns SysTick */
#define configCPU_CLOCK_HZ                       (16000000UL) /* HSI reset default - see this file's own header */
#define configTICK_RATE_HZ                       ((TickType_t)1000)
#define configMAX_PRIORITIES                     (5)
#define configMINIMAL_STACK_SIZE                 ((unsigned short)128) /* words */
#define configTOTAL_HEAP_SIZE                    ((size_t)(16 * 1024)) /* 16 KB, out of this board's own 96 KB SRAM1+2 region - see STM32G474RETx_APP.ld */
#define configMAX_TASK_NAME_LEN                  (16)
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configQUEUE_REGISTRY_SIZE                8
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                (3)
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             (configMINIMAL_STACK_SIZE)
#define configSUPPORT_DYNAMIC_ALLOCATION          1
#define configSUPPORT_STATIC_ALLOCATION          0
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             1
#define configUSE_TRACE_FACILITY                 0

#define configUSE_TASK_NOTIFICATIONS             1
#define configUSE_MUTEXES                        1

/* Cortex-M4F NVIC priority config - standard STM32 values (4 priority bits). */
#define configPRIO_BITS                          4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY  15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY          (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Optional API functions this skeleton doesn't need yet - trimmed to keep
 * flash usage down, same "only what's used" philosophy as the HAL module
 * list in ../../build_firmware.sh. Add back as real code needs them. */
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1

#define configASSERT(x) if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;); }

/* Route FreeRTOS's own Cortex-M port handlers onto the exact vector-table
 * names the CMSIS startup file (startup_stm32g474xx.s) declares - this is
 * why STM32G474RE_main.c does NOT define its own SVC_Handler/PendSV_Handler/
 * SysTick_Handler once FreeRTOS is linked in: these macros make port.c's own
 * definitions BE those symbols, not just call them. */
#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
