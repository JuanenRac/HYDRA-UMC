# Analog values of the H757 board: what is set and what is only proposed

The numbers below come from the design of the board (the resistors of the schematic guide and the CubeMX project). None has been measured on a board: the evidence is the design plus the host tests (`evidence = simulator`), and every value stays a candidate until the bench test next to it has run.

## Set in `config/dac.cfg` (AO1 and AO2)

| Key | Value | Why |
|---|---|---|
| `gain_num` / `gain_den` | 186 / 61 | The amplifier is non-inverting with 20.0 k in the feedback and 9.76 k to ground: 1 + 20.0 / 9.76 = 186 / 61 = 3.049 (the earlier 3 / 1 read the output 1.6 % low). |
| `ao1_mv` / `ao2_mv` | 9900 | The ceiling stays under 10 V until the gain is measured: the full code would give 10.06 V, and with 0.1 % resistors at their worst the gain reaches 3.053, so 9.9 V asked can be 9.913 V at the output. |

`tests/test_dac_plan.py` keeps the arithmetic: the highest accepted code stays under 10 V with the worst resistors, and the full code would not.

Bench test (pending): with AO1 and AO2 unloaded and with 10 k, a six-digit multimeter and a scope: (1) 0 V at power-up and during reset; (2) steps 0, 2.5, 5, 7.5 and 9.9 V, measured against asked, the real gain being the slope of a linear fit; (3) the highest accepted code never above 10.000 V at 0 and 70 C; (4) with the field 24 V cut the output falls to 0 V; (5) only then write the measured gain and raise the ceiling to 10000.

## Set in `config/fdcan.cfg`, `config/fdcan2.cfg` and the `h757_target` profile (stack bus)

Prescaler 5, segment 1 of 16, segment 2 of 3 and a jump width of 3 at the 100 MHz kernel clock: 20 quanta of 50 ns, 1 Mbit/s exactly, sample point 85 %. A jump width may not be longer than segment 2, so the earlier 4 (which the plan cut to 3 anyway) is now 3 everywhere. `tests/test_fdcan_timing.py` checks the rate and that the jump width is not longer than segment 2 in the profile, the generated header and both config files.

Bench test (pending): measure the bit time (1.000 us) and the sample point (85 %) with an analyser, run ten minutes of traffic at more than 50 % load with eight nodes and read zero error and bus-off counters, then push one node's clock by 0.5 % and see that the bus keeps running up to about 0.58 %.

## The display panel (ILI9881)

The panel connector carries the two data lanes, the clock, the I2C pair and the 3.3 V supply of the display; the reset and interrupt nets of the H757 (PC15 and PC14) are spare nets that are not wired to the connector on this revision. **There is no reset line for the panel from the H757**: the profile no longer calls PC15 a reset, and the panel is expected to reset itself on the supply or through its own board. The vendor register pages of the controller are not in the repository (`step01` and `step02` of `config/panel.cfg` are only sleep-out and display-on); the table waits for the data sheet of the panel maker, with its licence checked, and nothing is invented.

Bench test (pending): scan the I2C bus of the display, check 3.3 V on the display supply and that PC15 has no continuity to the connector, load the table of the maker and see a stable picture for ten minutes, and confirm that with only sleep-out and display-on there is no picture.

## Proposed and not applied: the analog inputs (`config/adc.cfg` is unchanged)

The ratios below follow the resistors of the design; `config/adc.cfg` keeps 1 / 1 until the schematic is drawn and the values are measured.

| Input | Proposed `num, den` | Note |
|---|---|---|
| `v24` | 11, 1 | divider 100 k / 10 k |
| `jetson` | 57, 10 | divider 47 k / 10 k |
| `usb5v`, `m2_5v`, `hailo`, `nvme`, `wifi`, `fan5v`, `ctrl5v` | 2, 1 | divider 10 k / 10 k |
| `ctrl3v3` | 147, 100 | divider 4.7 k / 10 k |
| `ai1`, `ai2` (voltage mode) | 321, 100 | divider 22.1 k / 10 k; in the 4-20 mA mode the shunt in parallel with 10 k is 162.3 ohm, so the mode needs its own entry |
| `i_v24_total` | 1, 20 | gain 20 and a 2 milliohm shunt: current in amperes is the sense millivolts divided by 2 |
| other currents | 1, 20 | sense voltage only: the shunt values are not fixed yet |

The channels of `ai1` (ADC3 channel 13, pin PH2) and `i_fan` (ADC3 channel 14, pin PH3) are the ones of the CubeMX project.

Bench test (pending): five points per voltage input against a multimeter, a known load per current input, the offset at zero current stored in `offset_mv`, and a check that PH2 does not move PH3 or PF10.
