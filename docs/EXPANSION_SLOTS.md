# The expansion slots: power sequence and fault handling

The board has three expansion slots (PCIe connectors) that hold the cards with the stepper drivers; the drivers are not on the base board. Each slot has its own fused 24 V supply with an enable line, a fault line from the fuse (low is a fault), a present line (low when a card is in) and a reset line (low holds the card in reset); the pins are in `docs/platform_profiles.json` (`expansion.slots`). The logic that drives them is `src/mcu_stm32h757/Common/expansion_slots.h`, written first in `tools/expansion_slots.py`. It reads two masks (cards present, fuses in fault), takes the time, and answers two masks (which enables are high, which resets are released); the driver that moves the pins is a separate step.

## Rules (all fail-safe)

* **Everything starts off.** Every slot is cut with its card held in reset until the first update, and a slot is powered only when its bit is set in `allowed_mask` of `config/expansion_slots.cfg` (empty by default) **and** a card is present. A slot that is not allowed never leaves OFF, whatever its lines say.
* **Power before reset.** The enable goes high with the card in reset. The fault line is ignored for `fault_blank_ms` (the inrush of the card), then it must stay quiet until `power_good_ms`; only then is reset released, and the slot is READY `reset_release_ms` later. A card is never out of reset without power.
* **A fault cuts the slot.** The fault line low for `fault_debounce_ms` in READY, or at all after the blanking time while the card powers up or comes out of reset, cuts the supply and holds the card in reset. The slot stays in FAULT until a person clears it **and** the fuse is quiet; after `max_faults` faults in one slot it is LOCKED and only an explicit unlock lets it try again.
* **Pulling the card** cuts the slot at once and returns it to ABSENT.

## Tests

`tests/test_expansion_slots.c` runs the C code against the scenarios of `tools/gen_expansion_slots_vectors.py`, among them four long runs of a fixed seed, all written from the Python reference, and checks at every step that no card is out of reset without power and that no slot outside the mask is powered. The configuration limits are checked on both sides. Nothing here has run on a board.

## Driver of the cards on SPI6

`src/mcu_stm32h757/CM7/TmcCardSpi6.c` is the HAL driver of SPI6 under the datagrams of `Common/tmc_card.h`: master, 8 bits, SPI mode 3 (the TMC5160 clocks on the second edge of an idle-high clock), prescaler 32 on the 100 MHz kernel clock (3.125 MHz, the clock of the profile), software chip select. PG3 is the strobe, PE5 and PE6 carry the slot number, PG12 to PG14 are the bus on alternate function 5. It ships off (`HYDRA_TMC_CARD_SPI6 0`: the unit is empty and no pin is touched). The settling time of the select lines and the transfer time-out are in `config/tmc_card.cfg` and the driver refuses values outside 1 to 1000. `tests/test_tmc_card_spi6.c` compiles the driver with a double of the HAL and checks the configuration it gives the peripheral, the order of the pins, the refusals and the abort of a failed datagram; a build step compiles it against the real HAL. Nothing has run on a board.

## Physical test (not done)

A board with a card in each slot and a logic analyser on the enable, fault, present and reset lines of the three slots: with `allowed_mask = 0` nothing is powered whatever is plugged in; with a slot allowed, the order is enable, a pause, reset released; shorting the output of a slot's fuse makes the fault line go low, the slot is cut and stays cut until it is cleared; the third fault locks it.
