/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h745/CM4/SpiApp.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Driver of the SPI slave service of the application of the CM4; see SpiApp.h and spi_app.h. The pins, the mode and the frame size are those of the
 * bootloader (boot/bootloader_main.c, MX_SPI1_Init), which this application takes over after the jump. Not run on a board: bench row 13 of
 * docs/BENCH_TESTS.md (the link at full rate) is the check that remains.
 * ========================================================================== */
#include "SpiApp.h"

#if HYDRA_SPI_APP

#include "FreeRTOS.h"
#include "task.h"
#include "stm32h7xx_hal.h"
#include "KinematicBrainCan.h"
#include "estop_can.h"

#define SPI_APP_DATA_READY_PORT GPIOA
#define SPI_APP_DATA_READY_PIN  GPIO_PIN_3
#define SPI_APP_TRANSFER_TIMEOUT_MS 20u

static SPI_HandleTypeDef hspi1_app;
static SpiApp_t g_spi_app;
static uint8_t g_rx_wire[SPI_APP_FRAME_SIZE];

SpiApp_t *SpiApp_Instance(void)
{
    return &g_spi_app;
}

/* A set-up call that fails leaves the CM5 with no way to reach the brain: the service does not start, and the rest of the application runs as before. */
static uint8_t SpiApp_HalInit(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    GPIO_InitTypeDef pins = {0};
    pins.Pin = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    pins.Mode = GPIO_MODE_AF_PP;
    pins.Pull = GPIO_NOPULL;
    pins.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    pins.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &pins);

    GPIO_InitTypeDef ready = {0};
    ready.Pin = SPI_APP_DATA_READY_PIN;
    ready.Mode = GPIO_MODE_OUTPUT_PP;
    ready.Pull = GPIO_NOPULL;
    ready.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SPI_APP_DATA_READY_PORT, &ready);
    HAL_GPIO_WritePin(SPI_APP_DATA_READY_PORT, SPI_APP_DATA_READY_PIN, GPIO_PIN_RESET);

    hspi1_app.Instance = SPI1;
    hspi1_app.Init.Mode = SPI_MODE_SLAVE;
    hspi1_app.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1_app.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1_app.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1_app.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1_app.Init.NSS = SPI_NSS_HARD_INPUT;
    hspi1_app.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1_app.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1_app.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    return (uint8_t)(HAL_SPI_Init(&hspi1_app) == HAL_OK);
}

static void SpiApp_Task(void *parameters)
{
    (void)parameters;
    for (;;) {
        HAL_GPIO_WritePin(SPI_APP_DATA_READY_PORT, SPI_APP_DATA_READY_PIN, GPIO_PIN_SET);
        if (HAL_SPI_TransmitReceive(&hspi1_app, g_spi_app.tx_wire, g_rx_wire, SPI_APP_FRAME_SIZE, SPI_APP_TRANSFER_TIMEOUT_MS) == HAL_OK) {
            (void)SpiApp_OnReceive(&g_spi_app, g_rx_wire);
        }
        HAL_GPIO_WritePin(SPI_APP_DATA_READY_PORT, SPI_APP_DATA_READY_PIN, GPIO_PIN_RESET);
        SpiApp_Service(&g_spi_app);
        vTaskDelay(1); /* lets the lower priorities run when the CM5 is silent (the transfer above then ends by its timeout) */
    }
}

void SpiApp_Start(void)
{
    const SpiSysConfig_t config = SpiSys_ConfigFromBuild();
    SpiApp_Init(&g_spi_app, &config, (uint8_t)HYDRA_SPI_REQUIRE_CRC);
    if (!SpiApp_HalInit()) {
        g_spi_app.link_ok = 0u;
        return;
    }
    xTaskCreate(SpiApp_Task, "spi", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 2, NULL);
}

/* The slots that answer the probe: the same AXIS_STATUS query as the round robin of the STACK A task, which tells an answering board (1) from a
 * silent one (0). The duplicate, misplaced and noisy verdicts of docs/SLOT_SCAN.md need the identification frames of the full scan and are not
 * produced here; the CM5 sees only absent and ok from this application. */
static void SpiApp_RunScan(uint8_t scan_id)
{
    uint8_t verdicts[8];
    for (uint8_t slot = 0; slot < 8u; slot++) {
        uint8_t status[8];
        uint8_t dlc = 0;
        verdicts[slot] = KinematicBrainCan_QueryAxisStatus(slot, status, &dlc, 20u) ? 1u : 0u;
        KinematicBrainCan_EstopPoll(); /* a long scan must not starve the stop frames */
    }
    SpiApp_ScanDone(&g_spi_app, scan_id, verdicts);
}

void SpiApp_Pump(void)
{
    uint8_t reason = 0, scan_id = 0;
    if (SpiApp_TakeTrip(&g_spi_app, &reason)) KinematicBrainCan_EstopTrip(reason);
    if (SpiApp_TakeRelease(&g_spi_app)) (void)KinematicBrainCan_EstopRequestRelease((uint8_t)HYDRA_SPI_RELEASE_EXPECTED_MASK);
    if (SpiApp_TakeScan(&g_spi_app, &scan_id)) SpiApp_RunScan(scan_id);
#if HYDRA_SPI_ZONES_HEARTBEAT
    if (SpiApp_ZonesTick(&g_spi_app, HAL_GetTick(), (uint32_t)HYDRA_SPI_ZONES_MAX_AGE_MS)) KinematicBrainCan_EstopTrip((uint8_t)SPI_APP_ZONES_REASON);
#endif

    /* published after the requests ran, so the telemetry that follows a stop already says the stop is latched */
    uint8_t release_state = 0, missing = 0;
    const uint8_t latched = KinematicBrainCan_EstopStatus(&release_state, &missing);
    uint8_t tx_errors = 0, rx_errors = 0;
    KinematicBrainCan_ErrorCounters(&tx_errors, &rx_errors);
    SpiApp_PublishBrainState(&g_spi_app, latched, tx_errors, rx_errors, g_spi_app.link_ok);
}

#else
typedef int spi_app_unit_is_empty_when_the_switch_is_off; /* a translation unit may not be empty */
#endif
