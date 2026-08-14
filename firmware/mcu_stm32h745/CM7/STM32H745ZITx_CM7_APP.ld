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
 * Flash Bank 1 layout (1 MB = 0x100000 total):
 *   0x08000000  Bootloader        64 KB (0x10000) - see boot/STM32H745ZITx_CM7_BOOTLOADER.ld
 *   0x08010000  Metadata           4 KB (0x1000)
 *   0x08011000  App main slot    476 KB (0x77000) - THIS script targets this slot
 *   0x08088000  App backup slot  476 KB (0x77000)
 *   0x080FF000  Reserved           4 KB (0x1000)
 */

MEMORY
{
  FLASH_APP (rx) : ORIGIN = 0x08011000, LENGTH = 476K
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
