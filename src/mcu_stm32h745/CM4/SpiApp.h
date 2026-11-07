/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h745/CM4/SpiApp.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The driver of the SPI slave service of the application of the CM4 (the logic is spi_app.h). With HYDRA_SPI_APP at 0, as it ships, this file
 * declares nothing the firmware calls and SpiApp.c compiles to an empty unit.
 *
 *   SpiApp_Start()      once from main(): SPI1 in slave mode on the pins of the bootloader (PA4 to PA7, DATA_READY on PA3) and the SPI task
 *   SpiApp_Instance()   the state shared with the STACK A task
 *   SpiApp_Pump()       one pass of the STACK A task: hands the state of the brain over to the SPI task, and runs what the SPI task asked for
 * ========================================================================== */
#ifndef HYDRA_UMC_SPI_APP_DRIVER_H
#define HYDRA_UMC_SPI_APP_DRIVER_H

#include "spi_app.h"

#if HYDRA_SPI_APP
#ifndef HYDRA_SPI_RELEASE_EXPECTED_MASK
/* Bit n = the board of slot n has to answer READY before a release ends the stop. All eight by default: a stack with an empty slot then stays
 * stopped (fail closed) until a build names the slots it really has. */
#define HYDRA_SPI_RELEASE_EXPECTED_MASK 0xFFu
#endif

void SpiApp_Start(void);
SpiApp_t *SpiApp_Instance(void);
void SpiApp_Pump(void);
#endif

#endif /* HYDRA_UMC_SPI_APP_DRIVER_H */
