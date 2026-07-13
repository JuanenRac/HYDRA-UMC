
----------------------------------------------------------------------
🚀 HYDRA-UMC: The Ultimate 12-Axis Dual-MCU Micro-Factory Controller
----------------------------------------------------------------------

HYDRA-UMC (Universal Multi-axis Controller) is an industrial-grade, ultra-scalable control platform powered by a synchronized dual-STM32 microcontroller architecture. Designed to overcome the processing bottlenecks of traditional single-MCU boards, it acts as the main brain of an automated manufacturing cell, seamlessly coordinating kinematics, logistics, tool-changing, and thermal processes.

----------------------------------------------------------------------
🏭 The Micro-Factory Ecosystem
----------------------------------------------------------------------
HYDRA-UMC is specifically engineered to orchestrate a modular and automated SMD assembly line:
* **JuanenPNP Integration**: Works in tandem with the Pick-and-Place machine (based on LumenPNP) to precisely position SMD components onto solder paste.
* **JuanenBOT (Parol6-based)**: Controls a 6-axis robotic arm running native Parol6 firmware. This arm is mounted on an extended XY gantry system to exponentially expand its working envelope.
* **Rotary ATC (Automatic Tool Changer)**: Manages a rotary tool-changing system to dynamically swap end-effectors on the JuanenBOT.
* **Logistics Line & Thermal Reflow**: Controls a dual-rack system for raw PCB entry, an output conveyor belt for finished boards, and a high-power heated bed to execute the SMD reflow soldering profile.
* **Multipurpose Modularity**: Thanks to its dual-MCU architecture, the board can be reconfigured to control two independent JuanenBOT arms simultaneously.

----------------------------------------------------------------------
🛠️ Dual-MCU Architecture & Axis Matrix
----------------------------------------------------------------------
The platform splits kinematics calculation, real-time step generation, safety diagnostics, and peripheral control between a Master MCU and an Auxiliary Co-processor, driving up to 12 independent high-power axes via SPI-configured TMC5160A-TA drivers:

1.  **Master Controller (U9 - STM32F446VET6 / LQFP-100)**:
    Handles high-level kinematics, motion file execution, high-speed networking, and the primary 6 axes of the system:
    * **Axis X**: Primary linear axis (TMC5160A U16).
    * **Axis Y1 & Y2**: Slipped-gantry synchronized dual-motor control (TMC5160A U17 / U18).
    * **Axis Z**: Vertical positioning axis (TMC5160A U19).
    * **Axis E0 & E1**: Tool-head actuators or primary extruders (TMC5160A U22 & U23).

2.  **Auxiliary Co-Processor (U24 - STM32F446RET6 / LQFP-64)**:
    Operates as a deterministic real-time expansion block to drive the remaining 6 axes:
    * **Axes 7 to 12 (Auxiliary)**: Drives independent stepper motors for XY positioning tables, conveyor belts, ATC systems, or Mecanum omnidirectional wheel coordination.
    * Manages real-time analog power telemetry and safety hardware loops.

----------------------------------------------------------------------
🔌 Advanced Connectivity & Bus Topology
----------------------------------------------------------------------
To mitigate electromagnetic interference (EMI) in harsh industrial environments, the board segregates its communication buses:
* **Dedicated Inter-MCU CAN Bus**: A private, isolated communication line exclusively for MCU1 and MCU2 to guarantee noise-free synchronization between robot kinematics and cell logistics.
* **External Tool CAN Bus (URTC)**: Designed to interface with the URTC tool-head controller boards located on the robot, keeping the 120-ohm termination resistors on the tool-heads to minimize umbilical cable weight.
* **Triple CAN Topology**:
    * **CAN3 (Master U9)**: Main industrial communication network of the system.
    * **CAN1 & CAN2 (Co-processor U24)**: Isolated CAN channels for tool-heads and secondary node clustering.
* **High-Speed W5500 Ethernet**: Hardware TCP/IP stack controller connected via a dedicated SPI2 bus (SPI2_ETH_CS) for low-latency host communication.
* **Integrated USB Hub (SL2.1A)**: Routes the USB_D/USB_D- differential lines to expose a single physical connection port from the PC to both microcontrollers.
* **MicroSD Storage**: Native 4-bit SDIO interface (SDIO_D0-D3, CMD, CLK) for local G-Code storage and system logging.

----------------------------------------------------------------------
⚡ Power, Safety & Industrial Peripherals
----------------------------------------------------------------------
* **High-Current Power Input**: Main supply provided via a rugged XT60PW-M connector designed to absorb massive current spikes without voltage drops.
* **High-Current SSR Bed Driver**: Dedicated hardware output (SSR_BED) for industrial AC/DC heated beds, ideal for SMD reflow profiling.
* **Power Rail Telemetry (VBUS_24V_ADC)**: High-precision voltage divider linked to the ADC for continuous monitoring of the main 24V bus.
* **Inductive Load Control**: MOSFET outputs for a hydraulic/vacuum pump (PUMP_24V), solenoid valve (VALVE_24V), and two independent PWM outputs for cooling fans (FAN1, FAN2).
* **Dual-Loop Hardware ESTOP**: Split emergency stop circuits completely independent of software (ESTOP1 on Co-processor, ESTOP2 on Master) for instantaneous, hardwired power shutdowns.
* **12 Endstop Inputs**: Dedicated pins (X1LIM, X2LIM, Y1LIM, Y2LIM, Z1LIM, Z2LIM, E0LIM, E1LIM, and LIMIT_AXIS1 to 6) protected with passive RC filters on the PCB to prevent false triggers from motor noise.
* **Raspberry Pi Compatible Expansion HAT**: Standard 40-pin style interface providing UART, I2C, SPI4, PWM, and 8 mapped GPIO channels (STM32_GP5-12) for high-level hosts like Klipper, ROS, or custom Linux platforms.
* **Diagnostic LEDs**: Visual diagnostic array including system status (LED_STATUS), error indicators (LED_DIAG_ERR), board-specific debugging LEDs (LED1, LED2), and a dedicated addressable Neopixel bus (RGB_HEAD).

----------------------------------------------------------------------
🔍 Current Status & Firmware Coordination
----------------------------------------------------------------------
* **Hardware**: Schematic and PCB routing are fully finalized under Eagle 9.5.2, taking extreme care to isolate high-frequency stepper driver switching noise from the sensitive analog sensing zones.
* **Firmware Layout**:
    * **MCU1 (Master U9)**: Runs an optimized port of the native PAROL6 firmware for robotic arm control, handling SDIO filesystem operations, Ethernet packets, and fast step generation for axes 1 to 6.
    * **MCU2 (Co-processor U24)**: Runs custom ultra-low latency firmware acting as a smart peripheral to drive auxiliary axes (7 to 12), sample analog rails, and coordinate cell-level automation tasks (ATC, conveyor logistics, and safety loops).
* **Current Phase**: Physical PCB prototypes are undergoing bench testing, validating the high-speed inter-MCU communication and optimizing network throughput over the W5500 Ethernet interface.
