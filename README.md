# HYDRA-UMC
HYDRA-UMC: The Ultimate 12-Axis Dual-MCU Universal Controller (for Parol6 and Faze4 robots)
-----------------------------------------------------------------------------
🚀 Introducing HYDRA-UMC: The Ultimate 12-Axis Dual-MCU Universal Controller
-----------------------------------------------------------------------------

Hi everyone! I wanted to share a major hardware project I've been developing called HYDRA-UMC (Universal Multi-axis Controller). It is an industrial-grade, ultra-scalable control platform powered by a synchronized dual-STM32 architecture. Designed specifically to handle advanced multi-axis CNC machines, heavy-load mobile transport platforms, custom pick-and-place systems (JuanenPNP), and complex multi-joint robotic setups that require massive independent motor control without compromises.

Here is the complete breakdown of what it is, what it does, and the dual-core hardware ecosystem it currently manages:

----------------------------------------------------------------------
⚙️ What is HYDRA-UMC?
----------------------------------------------------------------------
HYDRA-UMC is an all-in-one, high-performance motherboard designed to eliminate the bottlenecks of traditional single-MCU control boards. By splitting the real-time processing loads across two separate STM32F446 microcontrollers communicating via independent, deterministic hardware buses, HYDRA-UMC can drive up to 12 independent high-power axes via SPI-configured TMC5160A-TA drivers. 

The board features native high-speed Ethernet, multiple isolated CAN Bus nodes, intelligent ATX power management, hardware-level emergency safety loops, and a dedicated expansion interface compatible with Raspberry Pi HAT layouts for advanced high-level kinematic processing.

----------------------------------------------------------------------
🛠️ Dual-MCU Architecture & Axis Matrix
----------------------------------------------------------------------
The core strength of HYDRA-UMC is its asymmetric multi-processing layout, dividing the execution of kinematics, step-generation, safety monitoring, and peripheral control between a Master MCU and an Auxiliary Co-processor:

1. Master Controller (U9 - STM32F446VET6 / LQFP-100):
   Manages the primary machine kinematics, file parsing, high-speed networking, and the first 6-axis block (Main Kinematics Layer).
   * Axis X: Primary linear axis (TMC5160A U16)
   * Axis Y1 & Y2: Dual-motor synchronized gantry control (TMC5160A U17 / U18)
   * Axis Z: Vertical positioning axis (TMC5160A U19)
   * Axis E0: Primary Extrusor / Tool Actuator 0 (TMC5160A U22)
   * Axis E1: Secondary Extrusor / Tool Actuator 1 (TMC5160A U23)

2. Auxiliary Co-Processor (U24 - STM32F446RET6 / LQFP-64):
   Operates as a deterministic real-time hardware expansion block managing the remaining 6 axes (Auxiliary Sub-Systems) and critical power/safety diagnostics.
   * Axis 1 to 6 (Auxiliary): Independent auxiliary motors for secondary gantries, multi-tool changers, complex robotic end-effectors, or dual-row Moebius omnidirectional wheel coordination.

----------------------------------------------------------------------
🔌 Core Hardware Specifications & Peripherals
----------------------------------------------------------------------
HYDRA-UMC integrates full industrial connectivity and high-current power stages directly onto a single multi-layer PCB layout:

1. Power & Thermal Management Stage:
   * ATX Power Supply Integration: Onboard control logic (SUPPLY_ON_OFF, SUPPLY_BUTTON_STATE) to command ATX power sequencing via hardware.
   * High-Current SSR Bed Driver: Dedicated hardware output (SSR_BED) for industrial AC/DC heated beds.
   * Dual Thermistor Channels: Precision ADC inputs (THERM_BED1, THERM_BED2) filtered for real-time thermal monitoring.
   * Power Rail Telemetry: Hardware voltage divider linked to ADC (VBUS_24V_ADC) for continuous 24V main bus monitoring.
   * Inductive Load Actuators: High-current MOSFET outputs for PUMP_24V, VALVE_24V, and dual PWM fan channels (FAN1, FAN2).

2. Networking & Hardware Communications:
   * W5500 High-Speed Ethernet: Hardwired TCP/IP stack controller driven via a dedicated SPI2 bus (SPI2_ETH_CS) for latency-free network operation.
   * Triple CAN Bus Topology: 
     * CAN3 (Master U9): Main system-wide industrial CAN network interface.
     * CAN1 & CAN2 (Co-processor U24): Secondary isolated CAN networks for auxiliary toolboards and node clustering.
   * Onboard USB Hub (SL2.1A): Integrates multi-channel USB routing (USB_D/USB_D-) to provide unified PC connection points for both MCUs.
   * MicroSD Storage: Native 4-bit SDIO interface (SDIO_D0-D3, CMD, CLK) for high-speed local G-code execution and data logging.

3. Safety, Diagnostics & Expansion Interface:
   * Dual-Loop Hardware ESTOP: Split emergency stop loops (ESTOP1 on Co-processor, ESTOP2 on Master) for instantaneous, non-software-dependent hardware shutdowns.
   * 12x Independent Endstop Inputs: Individual hardware limit inputs (X1LIM, X2LIM, Y1LIM, Y2LIM, Z1LIM, Z2LIM, E0LIM, E1LIM and LIMIT_AXIS1 to 6) with onboard hardware RC filtering.
   * Raspberry Pi Compatible Expansion HAT: Dedicated 40-pin style interface providing seamless UART, I2C, SPI4, PWM, and 8 mapped GPIO channels (STM32_GP5-12) for Klipper, ROS, or custom Linux-based control hosts.
   * Visual Diagnostics: Multi-channel LED array including LED_STATUS, LED_DIAG_ERR, board-specific LEDs (LED1, LED2), and a dedicated addressable Neopixel bus (RGB_HEAD).

----------------------------------------------------------------------
🔍 Current Status & Firmware Coordination
----------------------------------------------------------------------
The HYDRA-UMC hardware schematic and board routing are fully finalized under Eagle 9.5.2, optimizing signal integrity for high-speed SPI lines driving the 12 TMC5160A chips and decoupling the analog sensing zones from the high-frequency switching noise of the stepper drivers.

The firmware development layout utilizes a dual-firmware topology:
* Master Firmware (U9): Manages network packets, G-code execution queues, SDIO filesystem indexing, Raspberry Pi UART/SPI communication, and fast step-generation loops for the primary 6 axes using hardware timers.
* Co-processor Firmware (U24): Mapped as an ultra-low latency SPI/CAN peripheral. It handles deterministic real-time step generation for axes 1-6, monitors power rail fluctuations via high-speed ADC sampling, and acts as the supervisor for the hardware safety loop.

Currently, physical PCB prototyping and first-article bench testing are underway, focusing on testing the cross-communication synchronization matrices between U9 and U24, alongside network throughput optimization over the W5500 Ethernet node.
