/*
 * =============================================================================
 * STM32H745ZITx_CM7_APP.ld - Kinematic Brain, Cortex-M7 application linker script
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * PROPOSED memory partitioning, modeled on the same bootloader+app+backup-
 * slot split URTC's own firmware proves out (see the sibling URTC repo) and
 * HYDRA-UMC's own G474 scripts already apply - scaled to this chip's own
 * per-core flash bank (1 MB) instead of re-deriving the pattern from
 * scratch. See docs/COMPILE_STM32H745.TXT and docs/architecture.md section
 * 2 (Tier 0 flashing is over SPI, not CAN, but reuses the same command
 * vocabulary and the same verify-before-copy anti-bricking discipline).
 *
 * STM32H745ZIT6 dual-core boot config used here: CM7 boots from Flash Bank 1
 * (0x08000000, 1 MB of the chip's 2 MB total - see CM4's own linker script
 * for Bank 2). CM7 RAM: this script uses AXI SRAM (D1 domain, 512 KB @
 * 0x24000000) for .data/.bss/heap/stack - NOT DTCM/ITCM (those are left
 * available for a future fast-path optimization, not mapped here - see
 * this file's own header for why that's deliberately out of scope for a
 * first skeleton).
 *
 * Flash Bank 1 layout (1 MB = 0x100000 total, 8 sectors x 128 KB each -
 * CORRECTED during CAN-OTA bootloader implementation: STM32H7 erases in
 * 128 KB SECTORS, not 2 KB pages like this project's F3/G4 boards, so
 * every boundary below is now sector-aligned, unlike the byte-arbitrary
 * split this table used to show. Metadata also moved to its OWN sector,
 * separate from the bootloader's - see boot/bootloader_common.h's own
 * METADATA_ADDR comment for why sharing a sector with the running
 * bootloader's code was a real hazard on this chip, not just untidy):
 *   0x08000000  Sector 0: Bootloader             128 KB - see boot/STM32H745ZITx_CM7_BOOTLOADER.ld
 *   0x08020000  Sector 1: Metadata                128 KB - see boot/bootloader_common.h
 *   0x08040000  Sectors 2-4: App main slot        384 KB - THIS script targets this slot
 *   0x080A0000  Sectors 5-7: App backup slot      384 KB
 */

MEMORY
{
  FLASH_APP (rx) : ORIGIN = 0x08040000, LENGTH = 384K
  RAM (rwx)       : ORIGIN = 0x24000000, LENGTH = 512K
}

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size = 0x2000;
_Min_Stack_Size = 0x2000;

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH_APP

  .text :
  {
    . = ALIGN(4);
    *(.text) *(.text*)
    *(.glue_7) *(.glue_7t) *(.eh_frame)
    KEEP (*(.init)) KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH_APP

  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) . = ALIGN(4); } >FLASH_APP
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH_APP
  .ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH_APP
  .preinit_array : { PROVIDE_HIDDEN (__preinit_array_start = .); KEEP (*(.preinit_array*)); PROVIDE_HIDDEN (__preinit_array_end = .); } >FLASH_APP
  .init_array : { PROVIDE_HIDDEN (__init_array_start = .); KEEP (*(SORT(.init_array.*))); KEEP (*(.init_array*)); PROVIDE_HIDDEN (__init_array_end = .); } >FLASH_APP
  .fini_array : { PROVIDE_HIDDEN (__fini_array_start = .); KEEP (*(SORT(.fini_array.*))); KEEP (*(.fini_array*)); PROVIDE_HIDDEN (__fini_array_end = .); } >FLASH_APP

  _sidata = LOADADDR(.data);
  .data : { . = ALIGN(4); _sdata = .; *(.data) *(.data*) . = ALIGN(4); _edata = .; } >RAM AT> FLASH_APP

  . = ALIGN(4);
  .bss :
  {
    _sbss = .; __bss_start__ = _sbss;
    *(.bss) *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .; __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /DISCARD/ : { libc.a ( * ) libm.a ( * ) libgcc.a ( * ) }
  .ARM.attributes 0 : { *(.ARM.attributes) }
}
