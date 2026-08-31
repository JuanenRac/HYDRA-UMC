/*
 * =============================================================================
 * RobotControllerRelay.h - Real FDCAN1 slot responder + Tier 2/3 relay tunnel
 * PROJECT: HYDRA-UMC (Robot Controller Board firmware, Tier 1 - STM32G474RET6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * This is the real gap docs/architecture.md section 6's own status table
 * flags explicitly: "the Robot Controller Board's own APPLICATION-side
 * relay logic (not its bootloader) that actually speaks to the URTC head
 * is still not written" - found and left as a real, named next step while
 * building the Tier 0 (Kinematic Brain) side of this exact tunnel
 * (src/mcu_stm32h745/CM4/KinematicBrainCan.c). This module is that side.
 *
 * Two real jobs, both driven off this board's own slot base ID
 * (CAN_ID_STACKA_BASE + N*STACKA_SLOT_WINDOW, bootloader_common.h):
 *   1. Answers AXIS_STATUS (+0x10) queries from Tier 0 with this board's
 *      own real endstop/fault GPIO state - docs/CANBUS_STM32G474.TXT's own
 *      documented txData[0]/txData[1] layout, matching KinematicBrainCan.c's
 *      own KinematicBrainCan_QueryAxisStatus() sender.
 *   2. RELAY_SEND (+0x12) / RELAY_RECV (+0x13): the generic, ID-agnostic
 *      tunnel to this robot's own URTC Tool Head over this board's SECOND
 *      FDCAN controller (FDCAN2, docs/PINOUT_STM32G474_ROBOT_CONTROLLER.TXT
 *      section 1b) - reassembles a real RELAY_SEND fragment pair into one
 *      real Classic CAN frame and forwards it on FDCAN2 addressed exactly
 *      as read from the payload (this board never interprets the target ID
 *      or its contents - opaque cargo, docs/architecture.md section 5),
 *      and buffers whatever FDCAN2 delivers back for RELAY_RECV to drain,
 *      oldest first. Same real fragmentation scheme as KinematicBrainCan.c
 *      and spi_bridge/relay_tunnel.py (both this session's own work,
 *      written to the same spec) - 2-byte target/source CAN ID (BE) +
 *      1-byte total DLC + up to 5 data bytes per fragment.
 *
 * Deliberately NOT implemented here: OFS_ENTER_BOOTLOADER (+0x00). Real
 * docs/CANBUS_STM32G474.TXT section 2 already names this as unimplemented
 * ("this board currently has no way to voluntarily re-enter its own
 * bootloader except via a physical reset") - resetting into this board's
 * own bootloader on command needs a real boot-selection mechanism (e.g. a
 * backup-domain register the bootloader checks on the next reset) that
 * doesn't exist anywhere in this project yet, and is a genuinely separate
 * concern from the relay tunnel this module exists for. Left as a real,
 * named gap rather than silently glossed over - see this project's own
 * docs/CANBUS_STM32G474.TXT section 2 for where it's tracked.
 */
#ifndef ROBOT_CONTROLLER_RELAY_H
#define ROBOT_CONTROLLER_RELAY_H

#include <stdint.h>

/* Real init: reads this board's own BOARD_ID[2:0] DIP switch (mirrors the
 * bootloader's own ReadSlotBaseId() - see this module's own .c file for
 * why that function is re-implemented here rather than linked from
 * boot/bootloader_main.c), brings up FDCAN1 (uplink, re-initialized by
 * this application the same way KinematicBrainCan_Init() re-initializes
 * FDCAN1 on Tier 0 - this application and the bootloader never run at the
 * same time) and FDCAN2 (downlink to the URTC head, new), and configures
 * the 6 endstop + 3 fault-sense GPIOs AXIS_STATUS reports on. Call once
 * from main(), before the scheduler starts. */
void RobotControllerRelay_Init(void);

/* One real polling pass: services a real AXIS_STATUS query, ENTER_
 * BOOTLOADER placeholder (see header above - intentionally a no-op today),
 * RELAY_SEND fragment, or RELAY_RECV drain request arriving on FDCAN1
 * (uplink), and buffers whatever FDCAN2 (downlink) delivered since the
 * last call. Call repeatedly from a real FreeRTOS task loop (see
 * STM32G474RE_main.c's own vRelayTask). Never blocks longer than one
 * real Classic CAN frame's worth of polling - safe to call every tick. */
void RobotControllerRelay_Poll(void);

#endif /* ROBOT_CONTROLLER_RELAY_H */
