/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM4/SpiApp.c
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
#include "fram_i2c.h"

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

/* The F-RAM of the main board (docs/FRAM_ERASE.md): the FM24CL64B U7 on I2C4, SCL on PD12 and SDA on PD13 (alternate function 4), address 0x50, 4.7 kOhm pull-ups
 * on the board (no internal pull). The requests arrive as SPI_APP_FRAM_REQUEST and are run by the STACK A task in SpiApp_Pump; the logic is fram_node.h
 * (switch HYDRA_FRAM_ERASE, off) and the driver fram_i2c.h. Between two chunks the bus layer sends the frames of the emergency stop that are due, so an
 * erase (a few seconds at 100 kHz) never starves the ALIVE frames. HYDRA_FRAM_I2C_TIMING is the TIMINGR value for the kernel clock of I2C4 and the speed
 * the build chooses; it has no default on purpose (a made-up value could run the bus out of spec): while it is 0 the driver is not created and the
 * answer is NO_CHIP. Not run on a board: bench rows 30 and 31 of docs/BENCH_TESTS.md. */
#ifndef HYDRA_FRAM_I2C_TIMING
#define HYDRA_FRAM_I2C_TIMING 0
#endif

static FramNode_t g_fram_node;
#if HYDRA_FRAM_ERASE && HYDRA_FRAM_I2C_TIMING
static I2C_HandleTypeDef hi2c4_fram;
static FramI2cBus_t g_fram_bus;
static FramI2c_t g_fram_i2c;
static FramDriver_t g_fram_driver;

static int SpiApp_FramTransfer(void *context, uint8_t address7, const uint8_t *tx, uint32_t tx_length, uint8_t *rx, uint32_t rx_length, uint32_t timeout_ms)
{
    (void)context;
    if (tx_length < 2u) return 1;
    const uint16_t memory_address = (uint16_t)(((uint16_t)tx[0] << 8) | tx[1]);
    const uint16_t device = (uint16_t)(address7 << 1);
    if (rx_length > 0u) {
        return HAL_I2C_Mem_Read(&hi2c4_fram, device, memory_address, I2C_MEMADD_SIZE_16BIT, rx, (uint16_t)rx_length, timeout_ms) == HAL_OK ? 0 : 1;
    }
    return HAL_I2C_Mem_Write(&hi2c4_fram, device, memory_address, I2C_MEMADD_SIZE_16BIT, (uint8_t *)(uintptr_t)&tx[2], (uint16_t)(tx_length - 2u), timeout_ms) == HAL_OK ? 0 : 1;
}

static void SpiApp_FramYield(void *context)
{
    (void)context;
    KinematicBrainCan_EstopPoll();
}

static const FramDriver_t *SpiApp_FramInit(void)
{
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_I2C4_CLK_ENABLE();
    GPIO_InitTypeDef pins = {0};
    pins.Pin = GPIO_PIN_12 | GPIO_PIN_13;
    pins.Mode = GPIO_MODE_AF_OD;
    pins.Pull = GPIO_NOPULL;
    pins.Speed = GPIO_SPEED_FREQ_LOW;
    pins.Alternate = GPIO_AF4_I2C4;
    HAL_GPIO_Init(GPIOD, &pins);
    hi2c4_fram.Instance = I2C4;
    hi2c4_fram.Init.Timing = (uint32_t)HYDRA_FRAM_I2C_TIMING;
    hi2c4_fram.Init.OwnAddress1 = 0u;
    hi2c4_fram.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c4_fram.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c4_fram.Init.OwnAddress2 = 0u;
    hi2c4_fram.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c4_fram.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c4_fram) != HAL_OK) return NULL;
    g_fram_bus.transfer = SpiApp_FramTransfer;
    g_fram_bus.yield = SpiApp_FramYield;
    g_fram_bus.context = NULL;
    FramI2c_Init(&g_fram_i2c, &g_fram_bus);
    FramI2c_MakeDriver(&g_fram_driver, &g_fram_i2c);
    return &g_fram_driver;
}
#else
static const FramDriver_t *SpiApp_FramInit(void)
{
    return NULL; /* the switch is off, or no timing is fixed: the node answers DISABLED or NO_CHIP and the bus is never touched */
}
#endif /* HYDRA_FRAM_ERASE && HYDRA_FRAM_I2C_TIMING */

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
    FramNode_Init(&g_fram_node, SpiApp_FramInit());
    if (!SpiApp_HalInit()) {
        g_spi_app.link_ok = 0u;
        return;
    }
    xTaskCreate(SpiApp_Task, "spi", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 2, NULL);
}

/* The slots that answer the probe: the same AXIS_STATUS query as the round robin of the STACK A task, which tells an answering board (1) from a
 * silent one (0). The duplicate, misplaced and noisy verdicts of docs/SLOT_SCAN.md need the identification frames of the full scan and are not
 * produced here; the CM5 sees only absent and ok from this application. */
#if HYDRA_SPI_SLOT_SCAN
#include "slot_scan_sweep.h"

static void SpiApp_BusWindow(void *context, uint8_t slot, uint8_t scan_id, SlotScanMaster *window)
{
    (void)context;
    KinematicBrainCan_ScanWindow(slot, scan_id, (uint32_t)HYDRA_SPI_SLOT_SCAN_WINDOW_MS, window);
}

/* The full scan of docs/SLOT_SCAN.md: eight windows, each with the probe, the replies and the error frames, and the five verdicts. */
static void SpiApp_RunScan(uint8_t scan_id)
{
    uint8_t verdicts[SLOT_SCAN_MASTER_SLOTS];
    SlotScanSweep_Run(SpiApp_BusWindow, NULL, scan_id, verdicts);
    SpiApp_ScanDone(&g_spi_app, scan_id, verdicts);
}
#else
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
#endif /* HYDRA_SPI_SLOT_SCAN */

void SpiApp_Pump(void)
{
    uint8_t reason = 0, scan_id = 0;
    if (SpiApp_TakeTrip(&g_spi_app, &reason)) KinematicBrainCan_EstopTrip(reason);
    if (SpiApp_TakeRelease(&g_spi_app)) (void)KinematicBrainCan_EstopRequestRelease((uint8_t)HYDRA_SPI_RELEASE_EXPECTED_MASK);
    if (SpiApp_TakeScan(&g_spi_app, &scan_id)) SpiApp_RunScan(scan_id);
    uint8_t fram_request[8], fram_reply[8];
    if (SpiApp_TakeFram(&g_spi_app, fram_request) && FramNode_Handle(&g_fram_node, FRAM_BRAIN_ID, 0u, fram_request, 8u, fram_reply)) SpiApp_FramDone(&g_spi_app, fram_reply);
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
