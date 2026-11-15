/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/DacOut.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The two 0 to 10 V analog outputs of the H757 board on the CM7 (AO1 on PA4 and AO2 on PA5, the two channels of DAC1 behind an amplifier). The outputs start at 0 V: both channels are
 * set to code 0 before they are started, and a voltage is set only after Common/dac_plan.h has checked it against the gain and the ceiling of config/dac.cfg. OFF by default: with
 * HYDRA_DAC_CM7 0 (as it ships) DacOut.c compiles to an empty unit and the pins are not touched. Not run on a board; the check that remains is a meter on each output.
 * ========================================================================== */
#ifndef HYDRA_UMC_DAC_OUT_H
#define HYDRA_UMC_DAC_OUT_H

#ifndef HYDRA_DAC_CM7
#define HYDRA_DAC_CM7 0
#endif

#if HYDRA_DAC_CM7

#include <stdint.h>

#include "stm32h7xx_hal.h"
#include "dac_plan.h"

#define DACO_OK 0
#define DACO_E_ARG 1      /* the configuration was refused by the plan or a pointer is missing */
#define DACO_E_NOT_OPEN 2 /* the converter was not started */
#define DACO_E_HAL 3      /* the HAL refused the set-up or a value */
#define DACO_E_CHANNEL 4  /* not output 1 or 2 */
#define DACO_E_RANGE 5    /* over the ceiling of the output, or out of the range of the converter */

/* Starts DAC1 with both outputs at 0 V. The configuration is kept for the later calls; it has to stay valid. */
int DacOut_Open(const DacpConfig *config);

/* Sets output `channel` (1 or 2) to `millivolts`. Nothing is written to the converter when the request is refused. */
int DacOut_SetMillivolts(unsigned channel, uint32_t millivolts);

#endif /* HYDRA_DAC_CM7 */

#endif /* HYDRA_UMC_DAC_OUT_H */
