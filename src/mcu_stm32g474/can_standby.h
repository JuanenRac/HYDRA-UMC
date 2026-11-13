/*
 * HYDRA-UMC - src/mcu_stm32g474/can_standby.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 *
 * The pins that put the two CAN transceivers of the robot controller board out of standby (low = awake).
 *
 * The package of the chip of this board (LQFP-64) has no PG7 and no PG8, so the standby lines are PC5 (CAN1) and PC8 (CAN2), as
 * docs/PINOUT_STM32G474_ROBOT_CONTROLLER.md section 1d says. Both firmwares (the bootloader and the application) take them from here, so
 * they cannot disagree. Evidence: the sources and the pinout document are compared by tests/test_g474_can_standby.py (simulator); the
 * transceivers waking on a real board is the bench row listed in the hardware pending list.
 */
#ifndef HYDRA_G474_CAN_STANDBY_H
#define HYDRA_G474_CAN_STANDBY_H

#include "stm32g4xx_hal.h"

#define HYDRA_CAN1_STBY_PORT GPIOC
#define HYDRA_CAN1_STBY_PIN  GPIO_PIN_5
#define HYDRA_CAN2_STBY_PORT GPIOC
#define HYDRA_CAN2_STBY_PIN  GPIO_PIN_8
#define HYDRA_CAN_STBY_CLK_ENABLE() __HAL_RCC_GPIOC_CLK_ENABLE()

#endif /* HYDRA_G474_CAN_STANDBY_H */
