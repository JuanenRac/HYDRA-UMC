/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/AdcReader.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The three ADCs of the H757 board on the CM7 for the voltages, the currents and the analog field inputs of the board (docs/platform_profiles.json, board_io): it starts an ADC in
 * 12 or 16 bits with the asynchronous clock, calibrates it, and reads one channel by polling, the average of a number of conversions. The meaning of the number (millivolts of
 * the signal) is Common/adc_plan.h and the channels are in config/adc.cfg. The pins stay in analog mode, which is how they come out of reset. OFF by default: with HYDRA_ADC_CM7 0 (as it
 * ships) AdcReader.c compiles to an empty unit. Not run on a board.
 * ========================================================================== */
#ifndef HYDRA_UMC_ADC_READER_H
#define HYDRA_UMC_ADC_READER_H

#ifndef HYDRA_ADC_CM7
#define HYDRA_ADC_CM7 0
#endif

#if HYDRA_ADC_CM7

#include <stdint.h>

#include "stm32h7xx_hal.h"
#include "adc_plan.h"

#define ADCR_OK 0
#define ADCR_E_ARG 1      /* the configuration or the channel was refused by the plan, or a pointer is missing */
#define ADCR_E_NOT_OPEN 2 /* the ADC was not started */
#define ADCR_E_HAL 3      /* the HAL refused the set-up, the calibration, the channel or the conversion */
#define ADCR_E_TIMEOUT 4  /* a conversion did not finish in time */
#define ADCR_E_RANGE 5    /* the signal does not fit a signed 32-bit number */

/* Starts ADC `adc` (1..3) with the resolution, the sampling time and the calibration; ADCR_OK or an error. */
int AdcReader_Open(uint8_t adc, const AdcpConfig *config);

/* Reads the average of config->samples conversions of the channel into `raw` (a count of the resolution of the config). The ADC has to be open with the same resolution. */
int AdcReader_ReadRaw(const AdcpConfig *config, const AdcpChannel *channel, uint32_t *raw);

/* ReadRaw and then the signal in millivolts. */
int AdcReader_ReadMillivolts(const AdcpConfig *config, const AdcpChannel *channel, int32_t *mv);

#endif /* HYDRA_ADC_CM7 */

#endif /* HYDRA_UMC_ADC_READER_H */
