# Renode platform of the STM32G474

Renode ships no platform description for the STM32G4 family, so `tools/renode/stm32g474.repl` is written by hand for the controller board, with the addresses of the reference manual RM0440. `tools/renode/stm32g474.resc` loads it (and a firmware image when `$bin` is set). The manual workflow `renode-g474.yml` downloads the pinned Renode release, checks its SHA-256 and loads the platform; it fails when Renode reports an error. Nothing of the firmware runs there yet.

| Declared | Address |
|---|---|
| Cortex-M4F, NVIC | `0xE000E000` |
| Flash, 512 KB | `0x08000000` |
| SRAM1 + SRAM2, 96 KB | `0x20000000` |
| CCM SRAM, 32 KB (and its alias) | `0x20018000`, `0x10000000` |
| FDCAN message RAM (memory only) | `0x4000A400` |
| USART1, SPI1, SPI2 | `0x40013800`, `0x40013000`, `0x40003800` |
| Independent watchdog | `0x40003000` |
| GPIO ports A to G | `0x48000000` to `0x48001800` |

Not modelled, because Renode has no model that matches the register layout of this chip: the clock tree (RCC), the flash controller registers and the FDCAN protocol engine (the three FDCAN units are not declared, only their message RAM). Code that waits on them sees a peripheral that does not answer. Evidence: simulator; the load of the platform is the only thing checked.
