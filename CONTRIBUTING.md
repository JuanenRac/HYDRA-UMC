# Contributing to HYDRA-UMC 🦾

We welcome contributions to the core firmware and hardware of the HYDRA-UMC platform.

## Technology Stack
- **Language**: C11.
- **Hardware**: STM32H745 (Dual-Core), STM32G474.
- **Protocol**: FDCAN, SPI (IPC).
- **Environment**: ARM GNU Toolchain (`arm-none-eabi-gcc`).

## Guidelines
1. **Safety First**: Never disable hardware watchdogs or thermal limits.
2. **Deterministic Code**: Avoid `malloc` in the real-time motion engine. Use static allocation or FreeRTOS pools.
3. **Firmware Consistency**: Follow the existing naming convention for HAL vs. LL drivers.
4. **Testing**: Any change to S-Curve profiles must be verified in simulation before touching real motors.
