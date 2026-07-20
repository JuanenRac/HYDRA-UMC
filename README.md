----------------------------------------------------------------------
🚀 HYDRA-UMC: The Ultimate 6+6-Axis Dual-Core Micro-Factory Controller 
----------------------------------------------------------------------

HYDRA-UMC (Universal Multi-axis Controller) is an industrial-grade, ultra-dense control platform powered by a synchronized dual-core ARM Cortex-M7/M4 architecture . Engineered on a custom 6-layer PCB to overcome electromagnetic interference (EMI) and communication bottlenecks, it acts as the centralized brain of an automated manufacturing cell, seamlessly coordinating kinematics, logistics, tool-changing, interactive touch displays, and thermal processes without relying on external host computers or secondary controller boards .

----------------------------------------------------------------------
🏭 The Micro-Factory Ecosystem 
----------------------------------------------------------------------
HYDRA-UMC is specifically designed to orchestrate complex, multi-robot automated assembly cells and advanced robotics ecosystems :
* **Dual-Parol6 Robotic Arm Control**: Natively drives and synchronizes TWO independent 6-axis Parol6 robotic arms simultaneously from a single board .
* **JuanenPNP Integration**: Works in tandem with custom Pick-and-Place systems (based on LumenPNP structures) to precisely position SMD components onto solder paste .
* **Rotary ATC (Automatic Tool Changer)**: Manages automated tool-changing systems to dynamically swap end-effectors on the robotic arms .
* **Logistics Line & Thermal Reflow**: Controls dual-rack systems for raw PCB entry, output conveyor belts for finished boards, and high-power heated beds to execute SMD reflow soldering profiles .
* **JuanenBOT & Mobile Platforms**: Highly scalable architecture capable of controlling heavy-duty omnidirectional transport platforms (like 4-wheeled mecanum tables) integrated into the workflow .

----------------------------------------------------------------------
🛠️ Centralized Dual-Core Architecture & 6+6-Axis Matrix 
----------------------------------------------------------------------
The platform upgrades from legacy dual-MCU setups to a single, ultra-powerful **STM32H757BIT6 (LQFP-208)** . This asymmetric dual-core processor splits critical real-time motor control from GUI and network management :

* **Core 1 (ARM Cortex-M7 @ 480 MHz)**: Dedicated exclusively to high-speed real-time tasks . Handles inverse kinematics calculations, multi-axis motion interpolation, step generation, and closed-loop safety diagnostics for both robotic arms .
* **Core 2 (ARM Cortex-M4 @ 240 MHz)**: Acts as an asynchronous co-processor managing the interactive touch display, TCP/IP stack communication, local filesystem, and CAN bus parsing without interrupting motor pulse timing .
* **12-Axis Power Stage**: Drives up to 12 independent high-power axes via SPI-configured **TMC5160A-TA** stepper drivers, providing ultra-silent operation and massive current capability for NEMA motors . Step generation is hardware-mapped to high-speed advanced timers (`TIM1`, `TIM2`, `TIM3`, `TIM4`) .

----------------------------------------------------------------------
🧠 Massive Memory & Touch Display Ecosystem 
----------------------------------------------------------------------
To provide a smooth, modern industrial user experience similar to next-gen interfaces (e.g., style UI at 60 FPS via LVGL), HYDRA-UMC incorporates an extensive memory array and dedicated high-speed display buses :
* **2 MB Internal Flash & 1 MB Internal SRAM**: Built-in dual-bank Flash memory and 1 MB of internal SRAM structured in independent domains, guaranteeing full hardware isolation and zero memory contention between Core 1 real-time kinematics and Core 2 UI/network execution.
* **MIPI DSI Display Interface**: Native differential serial bus (`DSI_D0P/N`, `DSI_D1P/N`, `DSI_CKP/N`) to drive modern 7-inch capacitive touch panels . This dedicated hardware frees up over 20 GPIO pins compared to legacy RGB parallel interfaces .
* **Touch Controller**: I2C2 interface (`PB10`/`PB11`) dedicated to the GT911 capacitive touch sensor .
* **32 MB SDRAM**: High-speed Flexible Memory Controller (FMC) 16-bit data bus mapped across Ports D, E, F, and G for framebuffers and complex trajectory caching .
* **32 MB QSPI Flash**: Memory-mapped external storage remapped outside Port F (`PB2`, `PB6`, and dedicated Port D/E lines) to store UI fonts, high-resolution graphics, and web server interfaces without FMC bus conflicts .
* **64 KB External EEPROM**: Built-in `FM24CL64` non-volatile storage on an isolated bus (`I2C4` on `PH11`/`PH12`) with GPIO Write-Protect (WP) for safe storage of machine calibration tables, kinematics offsets, and runtime hours .

----------------------------------------------------------------------
🔌 Advanced Connectivity & Bus Topology 
----------------------------------------------------------------------
HYDRA-UMC eliminates external bridge ICs in favor of native, high-bandwidth internal peripherals :
* **Native Ethernet MAC (RMII)**: Built-in 10/100 Ethernet hardware stack mapped across Port A, C, and G, completely replacing external W5500 SPI chips for ultra-low latency host communication .
* **Native USB OTG Full-Speed**: Built-in PHY hardware (`PA11`/`PA12`) for direct USB-C connection to stream G-code or live telemetry .
* **Dedicated FDCAN1 Bus (URTC Tool-Head Link)**: High-speed Controller Area Network bus remapped to `PB8` (Rx) and `PB9` (Tx) to eliminate USB hardware conflicts . Engineered to interface directly with remote **URTC tool-head boards** (which include onboard FRAM storage and NEMA controllers) .
* **40-Pin Expansion Interface**: Raspberry Pi compatible header exposing isolated UART, I2C4, SPI, PWM, and GPIO channels for external single-board computers, sensors, or ROS nodes .

----------------------------------------------------------------------
⚡ Power, 6-Layer PCB Stackup & Industrial Protection 
----------------------------------------------------------------------
* **6-Layer PCB Stackup**: Designed under standard industrial EMI shielding rules to isolate 480 MHz switching signals from high-current motor noise :
    * *Layer 1 (Top)*: High-speed signals, oscillator, differential Ethernet RMII, and FPC connectors .
    * *Layer 2 (GND)*: Continuous, solid copper ground plane shield .
    * *Layer 3 (Signal)*: Blind internal routing for dense FMC SDRAM and MIPI DSI buses between two GND shields .
    * *Layer 4 (Power)*: Split power planes (3.3V, 1.2V core, 5V) .
    * *Layer 5 (GND)*: Secondary solid copper ground plane for bottom-layer return paths .
    * *Layer 6 (Bottom)*: High-current power routing for TMC5160 drivers and heavy actuators .
* **14 Endstop & Safety Inputs**: Dedicated limit switch pins protected with passive hardware RC filters (series resistor + capacitor to GND) physically placed near the STM32 to absorb noise spikes from long robot wiring looms .
* **High-Power Actuator Stage**: MOSFET outputs for hydraulic/vacuum pumps, solenoid valves, PWM cooling fans, and high-current Solid State Relay (SSR) driver for SMD reflow heated beds .

----------------------------------------------------------------------
🔍 Current Status & Development Phase 
----------------------------------------------------------------------
* **Hardware Validation**: Architecture, pin-mapping, and bus coexistence (FMC vs. QSPI vs. FDCAN vs. USB) have been fully validated for the LQFP-208 package . PCB layout and 6-layer routing are in progress .
* **Firmware Coordination**: Developing bare-metal/RTOS firmware leveraging the STM32H7 dual-core architecture . Core 1 runs an optimized dual-arm port of PAROL6 kinematics, while Core 2 manages the LVGL touch interface, Ethernet communications, and URTC tool-head CAN telemetry .
